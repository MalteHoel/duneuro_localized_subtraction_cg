#ifndef DUNEURO_FITTED_MEEG_DRIVER_HH
#define DUNEURO_FITTED_MEEG_DRIVER_HH

#include <dune/common/std/memory.hh>

#include <duneuro/common/cg_solver.hh>
#include <duneuro/common/default_grids.hh>
#include <duneuro/common/dg_solver.hh>
#include <duneuro/common/flags.hh>
#if HAVE_DUNE_SUBGRID
#include <duneuro/common/geometry_adaption.hh>
#endif
#include <duneuro/common/fitted_driver_data.hh>
#include <duneuro/common/grid_function_mean.hh>
#include <duneuro/common/matrix_utilities.hh>
#include <duneuro/common/stl.hh>
#include <duneuro/common/volume_conductor.hh>
#include <duneuro/common/volume_conductor_storage.hh>
#include <duneuro/eeg/cg_source_model_factory.hh>
#include <duneuro/eeg/conforming_eeg_forward_solver.hh>
#include <duneuro/eeg/conforming_transfer_matrix_solver.hh>
#include <duneuro/eeg/conforming_transfer_matrix_user.hh>
#include <duneuro/eeg/dg_source_model_factory.hh>
#include <duneuro/eeg/electrode_projection_factory.hh>
#include <duneuro/io/volume_conductor_reader.hh>
#include <duneuro/io/vtk_functors.hh>
#include <duneuro/io/vtk_writer.hh>
#include <duneuro/meeg/meeg_driver_interface.hh>
#include <duneuro/meg/conforming_meg_transfer_matrix_solver.hh>
#include <duneuro/meg/meg_solution.hh>

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

  template <int dim, ElementType elementType, FittedSolverType solverType, int degree,
            bool geometryAdaption>
  struct FittedMEEGDriverTraits {
    using VCStorage = VolumeConductorStorage<dim, elementType, geometryAdaption>;
    using VC = typename VCStorage::Type;
    using Solver = typename SelectFittedSolver<solverType, VC, elementType, degree>::SolverType;
    using SourceModelFactory =
        typename SelectFittedSolver<solverType, VC, elementType, degree>::SourceModelFactoryType;
    using DomainDOFVector = typename Solver::Traits::DomainDOFVector;
    using ElementSearch = KDTreeElementSearch<typename VC::GridView>;
  };

  template <int dim, ElementType elementType, FittedSolverType solverType, int degree,
            bool geometryAdaption = false>
  class FittedMEEGDriver : public MEEGDriverInterface<dim>
  {
  public:
    using Traits = FittedMEEGDriverTraits<dim, elementType, solverType, degree, geometryAdaption>;

    explicit FittedMEEGDriver(const Dune::ParameterTree& config, DataTree dataTree = DataTree())
        : FittedMEEGDriver(FittedDriverData<dim>{}, config, dataTree)
    {
    }

    explicit FittedMEEGDriver(const FittedDriverData<dim>& data, const Dune::ParameterTree& config,
                              DataTree dataTree = DataTree())
        : config_(config)
        , volumeConductorStorage_(data, config.sub("volume_conductor"),
                                  dataTree.sub("volume_conductor"))
        , elementSearch_(std::make_shared<typename Traits::ElementSearch>(
              volumeConductorStorage_.get()->gridView()))
        , solver_(std::make_shared<typename Traits::Solver>(
              volumeConductorStorage_.get(),
              config.hasSub("solver") ? config.sub("solver") : Dune::ParameterTree()))
        , eegForwardSolver_(volumeConductorStorage_.get(), elementSearch_, solver_)
        , eegTransferMatrixSolver_(volumeConductorStorage_.get(), solver_)
        , transferMatrixUser_(volumeConductorStorage_.get(), elementSearch_, solver_)
        , megTransferMatrixSolver_(volumeConductorStorage_.get(), solver_)
    {
    }

    virtual void solveEEGForward(const typename MEEGDriverInterface<dim>::DipoleType& dipole,
                                 Function& solution, const Dune::ParameterTree& config,
                                 DataTree dataTree = DataTree()) override
    {
      eegForwardSolver_.bind(dipole, dataTree);
      eegForwardSolver_.solve(solution.cast<typename Traits::DomainDOFVector>(), config, dataTree);
      if (config.get<bool>("post_process")) {
        eegForwardSolver_.postProcessSolution(solution.cast<typename Traits::DomainDOFVector>());
      }
      if (config.get<bool>("subtract_mean")) {
        subtract_mean(*solver_, solution.cast<typename Traits::DomainDOFVector>());
      }
    }

    virtual std::vector<double> solveMEGForward(const Function& eegSolution,
                                                const Dune::ParameterTree& config,
                                                DataTree dataTree = DataTree()) override
    {
      if (!megSolution_) {
        DUNE_THROW(Dune::Exception, "please call setCoilsAndProjections before solving meg");
      }
      return flatten(megSolution_->evaluate(eegSolution.cast<typename Traits::DomainDOFVector>()));
    }

    virtual std::unique_ptr<Function> makeDomainFunction() const override
    {
      return Dune::Std::make_unique<Function>(make_domain_dof_vector(*solver_, 0.0));
    }

    virtual void
    setElectrodes(const std::vector<typename MEEGDriverInterface<dim>::CoordinateType>& electrodes,
                  const Dune::ParameterTree& config) override
    {
      assert(electrodes.size() > 0);
      electrodeProjection_ = ElectrodeProjectionFactory::make_electrode_projection(
          config, volumeConductorStorage_.get()->gridView());
      electrodeProjection_->setElectrodes(electrodes);
      projectedGlobalElectrodes_.clear();
      for (unsigned int i = 0; i < electrodeProjection_->size(); ++i) {
        const auto& proj = electrodeProjection_->getProjection(i);
        projectedGlobalElectrodes_.push_back(proj.element.geometry().global(proj.localPosition));
      }
    }

    virtual void setCoilsAndProjections(
        const std::vector<typename MEEGDriverInterface<dim>::CoordinateType>& coils,
        const std::vector<std::vector<typename MEEGDriverInterface<dim>::CoordinateType>>&
            projections) override
    {
      if (coils.size() != projections.size()) {
        DUNE_THROW(Dune::Exception,
                   "number of coils (" << coils.size() << ") does not match number of projections ("
                                       << projections.size() << ")");
      }
      coils_ =
          Dune::Std::make_unique<std::vector<typename MEEGDriverInterface<dim>::CoordinateType>>(
              coils);
      projections_ = Dune::Std::
          make_unique<std::vector<std::vector<typename MEEGDriverInterface<dim>::CoordinateType>>>(
              projections);
      megSolution_ = Dune::Std::make_unique<MEGSolution<
          typename Traits::VC, typename Traits::Solver::Traits::FunctionSpace,
          typename Traits::Solver::Traits::RangeDOFVector::field_type>>(
          volumeConductorStorage_.get(), eegForwardSolver_.functionSpace(), *coils_, *projections_,
          config_.sub("meg"));
    }

    virtual std::vector<double> evaluateAtElectrodes(const Function& function) const override
    {
      // create discrete grid function
      using DGF =
          Dune::PDELab::DiscreteGridFunction<typename Traits::Solver::Traits::FunctionSpace::GFS,
                                             typename Traits::DomainDOFVector>;
      DGF dgf(eegForwardSolver_.functionSpace().getGFS(),
              function.cast<typename Traits::DomainDOFVector>());

      // evalaute discrete grid function at every projection
      std::vector<double> result;
      result.reserve(electrodeProjection_->size());
      for (std::size_t i = 0; i < electrodeProjection_->size(); ++i) {
        const auto& projection = electrodeProjection_->getProjection(i);
        typename DGF::Traits::RangeType y(0.0);
        dgf.evaluate(projection.element, projection.localPosition, y);
        result.push_back(y);
      }
      return result;
    }

    virtual void write(const Function& function, const Dune::ParameterTree& config,
                       DataTree dataTree = DataTree()) const override
    {
      auto format = config.get<std::string>("format");
      if (format == "vtk") {
        VTKWriter<typename Traits::VC, degree> writer(
            volumeConductorStorage_.get(), config.get<unsigned int>("subsampling", degree - 1));
        auto gradient_type = config.get<std::string>("gradient.type", "vertex");
        auto potential_type = config.get<std::string>("potential.type", "vertex");

        if (gradient_type == "vertex") {
          writer.addVertexDataGradient(
              eegForwardSolver_,
              Dune::stackobject_to_shared_ptr(function.cast<typename Traits::DomainDOFVector>()),
              "gradient_potential");
        } else {
          writer.addCellDataGradient(
              eegForwardSolver_,
              Dune::stackobject_to_shared_ptr(function.cast<typename Traits::DomainDOFVector>()),
              "gradient_potential");
        }
        if (potential_type == "vertex") {
          writer.addVertexData(
              eegForwardSolver_,
              Dune::stackobject_to_shared_ptr(function.cast<typename Traits::DomainDOFVector>()),
              "potential");
        } else {
          writer.addCellData(
              eegForwardSolver_,
              Dune::stackobject_to_shared_ptr(function.cast<typename Traits::DomainDOFVector>()),
              "potential");
        }
        writer.addCellData(std::make_shared<duneuro::TensorFunctor<typename Traits::VC>>(
            volumeConductorStorage_.get()));
        writer.write(config.get<std::string>("filename"), dataTree);
      } else {
        DUNE_THROW(Dune::Exception, "Unknown format \"" << format << "\"");
      }
    }

    virtual void write(const Dune::ParameterTree& config,
                       DataTree dataTree = DataTree()) const override
    {
      auto format = config.get<std::string>("format");
      if (format == "vtk") {
        VTKWriter<typename Traits::VC, degree> writer(
            volumeConductorStorage_.get(), config.get<unsigned int>("subsampling", degree - 1));
        writer.addCellData(std::make_shared<duneuro::TensorFunctor<typename Traits::VC>>(
            volumeConductorStorage_.get()));
        writer.write(config.get<std::string>("filename"), dataTree);
      } else {
        DUNE_THROW(Dune::Exception, "Unknown format \"" << format << "\"");
      }
    }

    virtual std::unique_ptr<DenseMatrix<double>>
    computeEEGTransferMatrix(const Dune::ParameterTree& config,
                             DataTree dataTree = DataTree()) override
    {
      auto solution = duneuro::make_domain_dof_vector(eegForwardSolver_, 0.0);
      auto transferMatrix = Dune::Std::make_unique<DenseMatrix<double>>(
          electrodeProjection_->size(), solution->flatsize());
      auto solver_config = config.sub("solver");
      for (unsigned int i = 1; i < electrodeProjection_->size(); ++i) {
        eegTransferMatrixSolver_.solve(
            electrodeProjection_->getProjection(0), electrodeProjection_->getProjection(i),
            *solution, solver_config, dataTree.sub("solver.electrode_" + std::to_string(i)));
        set_matrix_row(*transferMatrix, i, Dune::PDELab::Backend::native(*solution));
      }
      return std::move(transferMatrix);
    }

    virtual std::unique_ptr<DenseMatrix<double>>
    computeMEGTransferMatrix(const Dune::ParameterTree& config,
                             DataTree dataTree = DataTree()) override
    {
      if (!(coils_ && projections_)) {
        DUNE_THROW(Dune::Exception,
                   "please call setCoilsAndProjections before computing the MEG transfer matrix");
      }
      auto solution = duneuro::make_domain_dof_vector(eegForwardSolver_, 0.0);
      std::size_t numberOfProjections = 0;
      for (const auto& p : *projections_)
        numberOfProjections += p.size();
      auto transferMatrix =
          Dune::Std::make_unique<DenseMatrix<double>>(numberOfProjections, solution->flatsize());
      unsigned int offset = 0;
      auto solver_config = config.sub("solver");
      for (unsigned int i = 0; i < coils_->size(); ++i) {
        auto coilDT = dataTree.sub("solver.coil_" + std::to_string(i));
        for (unsigned int j = 0; j < (*projections_)[i].size(); ++j) {
          megTransferMatrixSolver_.solve((*coils_)[i], (*projections_)[i][j], *solution,
                                         solver_config,
                                         coilDT.sub("projection_" + std::to_string(j)));
          set_matrix_row(*transferMatrix, offset + j, Dune::PDELab::Backend::native(*solution));
        }
        offset += (*projections_)[i].size();
      }
      return std::move(transferMatrix);
    }

    virtual std::vector<double>
    applyEEGTransfer(const DenseMatrix<double>& transferMatrix,
                     const typename MEEGDriverInterface<dim>::DipoleType& dipole,
                     const Dune::ParameterTree& config, DataTree dataTree = DataTree()) override
    {
      transferMatrixUser_.bind(dipole, dataTree);
      auto result = transferMatrixUser_.solve(transferMatrix, dataTree);
      if (config.get<bool>("post_process")) {
        transferMatrixUser_.postProcessPotential(projectedGlobalElectrodes_, result);
      }
      if (config.get<bool>("subtract_mean")) {
        subtract_mean(result);
      }
      return result;
    }

    virtual void setSourceModel(const Dune::ParameterTree& config,
                                DataTree dataTree = DataTree()) override
    {
      transferMatrixUser_.setSourceModel(config, dataTree);
      eegForwardSolver_.setSourceModel(config, dataTree);
    }

    virtual std::vector<double>
    applyMEGTransfer(const DenseMatrix<double>& transferMatrix,
                     const typename MEEGDriverInterface<dim>::DipoleType& dipole,
                     const Dune::ParameterTree& config, DataTree dataTree = DataTree()) override
    {
      transferMatrixUser_.bind(dipole, dataTree);
      return transferMatrixUser_.solve(transferMatrix, dataTree);
    }

  private:
    Dune::ParameterTree config_;
    typename Traits::VCStorage volumeConductorStorage_;
    std::shared_ptr<typename Traits::ElementSearch> elementSearch_;
    std::shared_ptr<typename Traits::Solver> solver_;
    ConformingEEGForwardSolver<typename Traits::Solver, typename Traits::SourceModelFactory>
        eegForwardSolver_;
    ConformingTransferMatrixSolver<typename Traits::Solver> eegTransferMatrixSolver_;
    ConformingTransferMatrixUser<typename Traits::Solver, typename Traits::SourceModelFactory>
        transferMatrixUser_;
    std::unique_ptr<MEGSolution<typename Traits::VC, typename Traits::Solver::Traits::FunctionSpace,
                                typename Traits::Solver::Traits::RangeDOFVector::field_type>>
        megSolution_;
    ConformingMEGTransferMatrixSolver<typename Traits::Solver> megTransferMatrixSolver_;
    std::unique_ptr<duneuro::ElectrodeProjectionInterface<typename Traits::VC::GridView>>
        electrodeProjection_;
    std::vector<typename duneuro::ElectrodeProjectionInterface<
        typename Traits::VC::GridView>::GlobalCoordinate>
        projectedGlobalElectrodes_;
    std::unique_ptr<std::vector<typename MEEGDriverInterface<dim>::CoordinateType>> coils_;
    std::unique_ptr<std::vector<std::vector<typename MEEGDriverInterface<dim>::CoordinateType>>>
        projections_;
  };
}

#endif // DUNEURO_FITTED_MEEG_DRIVER_HH