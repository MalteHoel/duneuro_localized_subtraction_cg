#ifndef DUNEURO_FITTED_EEG_DRIVER_HH
#define DUNEURO_FITTED_EEG_DRIVER_HH

#include <dune/common/std/memory.hh>

#include <duneuro/common/cg_solver.hh>
#include <duneuro/common/default_grids.hh>
#include <duneuro/common/dg_solver.hh>
#include <duneuro/common/flags.hh>
#include <duneuro/common/geometry_adaption.hh>
#include <duneuro/common/stl.hh>
#include <duneuro/common/volume_conductor.hh>
#include <duneuro/eeg/cg_source_model_factory.hh>
#include <duneuro/eeg/conforming_eeg_forward_solver.hh>
#include <duneuro/eeg/conforming_transfer_matrix_solver.hh>
#include <duneuro/eeg/conforming_transfer_matrix_user.hh>
#include <duneuro/eeg/dg_source_model_factory.hh>
#include <duneuro/eeg/eeg_driver_interface.hh>
#include <duneuro/eeg/projected_electrodes.hh>
#include <duneuro/io/volume_conductor_reader.hh>
#include <duneuro/io/vtk_functors.hh>
#include <duneuro/io/vtk_writer.hh>

namespace duneuro
{
  template <FittedSolverType solverType, class VC, ElementType et, int degree>
  struct SelectFittedSolver;

  template <class VC, ElementType et, int degree>
  struct SelectFittedSolver<FittedSolverType::cg, VC, et, degree> {
    using SolverType = CGSolver<VC, et, degree>;
    using SourceModelFactoryType = CGSourceModelFactory;
  };

  template <class VC, ElementType et, int degree>
  struct SelectFittedSolver<FittedSolverType::dg, VC, et, degree> {
    using SolverType = DGSolver<VC, et, degree>;
    using SourceModelFactoryType = DGSourceModelFactory;
  };

  template <ElementType elementType, bool geometryAdaption>
  class VolumeConductorStorage;

  template <ElementType elementType>
  class VolumeConductorStorage<elementType, false>
  {
  public:
    using Type = VolumeConductor<typename DefaultGrid<elementType>::GridType>;

    explicit VolumeConductorStorage(const Dune::ParameterTree& config,
                                    DataTree dataTree = DataTree())
        : volumeConductor_(VolumeConductorReader<typename Type::GridType>::read(config, dataTree))
    {
    }

    std::shared_ptr<Type> get() const
    {
      assert(volumeConductor_);
      return volumeConductor_;
    }

  private:
    std::shared_ptr<Type> volumeConductor_;
  };

  template <>
  class VolumeConductorStorage<ElementType::hexahedron, true>
  {
  public:
    using Type = VolumeConductor<typename GeometryAdaptedGrid<3>::GridType>;

    explicit VolumeConductorStorage(const Dune::ParameterTree& config,
                                    DataTree dataTree = DataTree())
        : adaptedGrid_(GeometryAdaptedGridReader<3>::read(config.sub("grid")))
        , volumeConductor_(make_geometry_adapted_volume_conductor<3>(
              std::move(adaptedGrid_.grid), std::move(adaptedGrid_.labels), config))
    {
    }

    std::shared_ptr<Type> get() const
    {
      assert(volumeConductor_);
      return volumeConductor_;
    }

  private:
    GeometryAdaptedGrid<3> adaptedGrid_;
    std::shared_ptr<Type> volumeConductor_;
  };

  template <ElementType elementType, FittedSolverType solverType, int degree, bool geometryAdaption>
  struct FittedEEGDriverTraits {
    using VCStorage = VolumeConductorStorage<elementType, geometryAdaption>;
    using VC = typename VCStorage::Type;
    using Solver = typename SelectFittedSolver<solverType, VC, elementType, degree>::SolverType;
    using SourceModelFactory =
        typename SelectFittedSolver<solverType, VC, elementType, degree>::SourceModelFactoryType;
    using DomainDOFVector = typename Solver::Traits::DomainDOFVector;
  };

  template <ElementType elementType, FittedSolverType solverType, int degree,
            bool geometryAdaption = false>
  class FittedEEGDriver : public EEGDriverInterface
  {
  public:
    using Traits = FittedEEGDriverTraits<elementType, solverType, degree, geometryAdaption>;

    explicit FittedEEGDriver(const Dune::ParameterTree& config, DataTree dataTree = DataTree())
        : volumeConductorStorage_(config.sub("volume_conductor"), dataTree.sub("volume_conductor"))
        , eegForwardSolver_(volumeConductorStorage_.get(), config.sub("solver"))
        , eegTransferMatrixSolver_(volumeConductorStorage_.get(), config.sub("solver"))
        , eegTransferMatrixUser_(volumeConductorStorage_.get(), config.sub("solver"))
    {
    }

    virtual void solve(const EEGDriverInterface::DipoleType& dipole, Function& solution,
                       DataTree dataTree = DataTree()) override
    {
      eegForwardSolver_.solve(dipole, solution.cast<typename Traits::DomainDOFVector>(), dataTree);
    }

    virtual Function makeDomainFunction() const override
    {
      return Function(make_shared_from_unique(make_domain_dof_vector(eegForwardSolver_, 0.0)));
    }

    virtual void
    setElectrodes(const std::vector<EEGDriverInterface::CoordinateType>& electrodes) override
    {
      projectedElectrodes_ =
          Dune::Std::make_unique<duneuro::ProjectedElectrodes<typename Traits::VC::GridView>>(
              electrodes, volumeConductorStorage_.get()->gridView());
    }

    virtual std::vector<double> evaluateAtElectrodes(const Function& function) const override
    {
      checkElectrodes();
      return projectedElectrodes_->evaluate(eegForwardSolver_.functionSpace().getGFS(),
                                            function.cast<typename Traits::DomainDOFVector>());
    }

    virtual void write(const Dune::ParameterTree& config, const Function& function,
                       const std::string& suffix = "") const override
    {
      auto format = config.get<std::string>("format");
      if (format == "vtk") {
        VTKWriter<typename Traits::VC, degree> writer(volumeConductorStorage_.get());
        writer.addVertexData(
            eegForwardSolver_,
            Dune::stackobject_to_shared_ptr(function.cast<typename Traits::DomainDOFVector>()),
            "potential");
        writer.addCellData(std::make_shared<duneuro::TensorFunctor<typename Traits::VC>>(
            volumeConductorStorage_.get()));
        writer.write(config.get<std::string>("filename") + suffix);
      } else {
        DUNE_THROW(Dune::Exception, "Unknown format \"" << format << "\"");
      }
    }

    virtual std::unique_ptr<DenseMatrix<double>>
    computeTransferMatrix(DataTree dataTree = DataTree()) override
    {
      checkElectrodes();
      auto solution = duneuro::make_domain_dof_vector(eegForwardSolver_, 0.0);
      auto transferMatrix = Dune::Std::make_unique<DenseMatrix<double>>(
          projectedElectrodes_->size(), solution->flatsize());
      for (unsigned int i = 1; i < projectedElectrodes_->size(); ++i) {
        eegTransferMatrixSolver_.solve(projectedElectrodes_->projectedPosition(0),
                                       projectedElectrodes_->projectedPosition(i), *solution,
                                       dataTree.sub("solver.electrode_" + std::to_string(i)));
        set_matrix_row(*transferMatrix, i, Dune::PDELab::Backend::native(*solution));
        std::cout << "solution:\n" << Dune::PDELab::Backend::native(*solution) << "\n\n";
      }
      return std::move(transferMatrix);
    }

    virtual std::vector<double> solve(const DenseMatrix<double>& transferMatrix,
                                      const DipoleType& dipole,
                                      DataTree dataTree = DataTree()) override
    {
      return eegTransferMatrixUser_.solve(transferMatrix, dipole, dataTree);
    }

  private:
    void checkElectrodes() const
    {
      if (!projectedElectrodes_) {
        DUNE_THROW(Dune::Exception, "electrodes not set");
      }
    }
    typename Traits::VCStorage volumeConductorStorage_;
    ConformingEEGForwardSolver<typename Traits::Solver, typename Traits::SourceModelFactory>
        eegForwardSolver_;
    ConformingTransferMatrixSolver<typename Traits::Solver> eegTransferMatrixSolver_;
    ConformingTransferMatrixUser<typename Traits::Solver, typename Traits::SourceModelFactory>
        eegTransferMatrixUser_;
    std::unique_ptr<duneuro::ProjectedElectrodes<typename Traits::VC::GridView>>
        projectedElectrodes_;
  };
}

#endif // DUNEURO_FITTED_EEG_DRIVER_HH
