#ifndef FITTED_VOLUME_CONDUCTOR_HH
#define FITTED_VOLUME_CONDUCTOR_HH

#if HAVE_TBB
#include <tbb/tbb.h>
#endif

#include <duneuro/common/cg_solver.hh>
#include <duneuro/common/cg_solver_backend.hh>
#include <duneuro/common/default_grids.hh>
#include <duneuro/common/dg_solver.hh>
#include <duneuro/common/dg_solver_backend.hh>
#include <duneuro/common/flags.hh>
#if HAVE_DUNE_SUBGRID
#include <duneuro/common/geometry_adaption.hh>
#endif
#include <duneuro/common/fitted_driver_data.hh>
#include <duneuro/common/grid_function_mean.hh>
#include <duneuro/common/matrix_utilities.hh>
#include <duneuro/common/stl.hh>
#include <duneuro/common/volume_conductor.hh>
#include <duneuro/common/volume_conductor_statistics.hh>
#include <duneuro/common/volume_conductor_storage.hh>
#include <duneuro/eeg/cg_source_model_factory.hh>
#include <duneuro/eeg/dg_source_model_factory.hh>
#include <duneuro/eeg/eeg_forward_solver.hh>
#include <duneuro/eeg/electrode_projection_factory.hh>
#include <duneuro/eeg/fitted_transfer_matrix_rhs_factory.hh>
#include <duneuro/eeg/transfer_matrix_solver.hh>
#include <duneuro/eeg/transfer_matrix_user.hh>
#include <duneuro/io/fitted_tensor_vtk_functor.hh>
#include <duneuro/io/volume_conductor_reader.hh>
#include <duneuro/io/volume_conductor_vtk_writer.hh>
#include <duneuro/io/vtk_writer.hh>
#include <duneuro/meg/fitted_meg_transfer_matrix_solver.hh>
#include <duneuro/meg/meg_solver_factory.hh>
#include <duneuro/meg/meg_solver_interface.hh>

#include <duneuro/driver/volume_conductor_interface.hh>
namespace duneuro {
template <FittedSolverType solverType, class VC, ElementType et, int degree>
struct SelectFittedSolver;

template <class VC, ElementType et, int degree>
struct SelectFittedSolver<FittedSolverType::cg, VC, et, degree> {
  using SolverType = CGSolver<VC, et, degree>;
  using SolverBackendType = CGSolverBackend<SolverType, et>;
  using SourceModelFactoryType = CGSourceModelFactory;
};

template <class VC, ElementType et, int degree>
struct SelectFittedSolver<FittedSolverType::dg, VC, et, degree> {
  using SolverType = DGSolver<VC, et, degree>;
  using SolverBackendType = DGSolverBackend<SolverType, et>;
  using SourceModelFactoryType = DGSourceModelFactory;
};

template <int dim, ElementType elementType, FittedSolverType solverType,
          int degree, bool geometryAdaption>
struct FittedMEEGDriverTraits {
  using VCStorage = VolumeConductorStorage<dim, elementType, geometryAdaption>;
  using VC = typename VCStorage::Type;
  using Solver = typename SelectFittedSolver<solverType, VC, elementType,
                                             degree>::SolverType;
  using SolverBackend = typename SelectFittedSolver<solverType, VC, elementType,
                                                    degree>::SolverBackendType;
  using SourceModelFactory =
      typename SelectFittedSolver<solverType, VC, elementType,
                                  degree>::SourceModelFactoryType;
  using TransferMatrixRHSFactory = FittedTransferMatrixRHSFactory;
  using DomainDOFVector = typename Solver::Traits::DomainDOFVector;
  using ElementSearch = KDTreeElementSearch<typename VC::GridView>;
  using TransferMatrixUser =
      duneuro::TransferMatrixUser<Solver, SourceModelFactory>;
};

template <int dim, ElementType elementType, FittedSolverType solverType,
          int degree, bool geometryAdaption = false>
class FittedVolumeConductor : public VolumeConductorInterface<dim> {
public:
  using Traits = FittedMEEGDriverTraits<dim, elementType, solverType, degree,
                                        geometryAdaption>;

  explicit FittedVolumeConductor(const Dune::ParameterTree &config,
                                 std::shared_ptr<FeatureManager> featureManager,
                                 DataTree dataTree = DataTree())
      : FittedVolumeConductor(FittedDriverData<dim>{}, config, featureManager, dataTree) {}

  explicit FittedVolumeConductor(const FittedDriverData<dim> &data,
                                 const Dune::ParameterTree &config,
                                 std::shared_ptr<FeatureManager> featureManager,
                                 DataTree dataTree = DataTree())
      : VolumeConductorInterface<dim>(featureManager),
        config_(config),
        volumeConductorStorage_(data, config.sub("volume_conductor"),
                                dataTree.sub("volume_conductor")),
        elementSearch_(std::make_shared<typename Traits::ElementSearch>(
            volumeConductorStorage_.get()->gridView())),
        solver_(std::make_shared<typename Traits::Solver>(
            volumeConductorStorage_.get(), elementSearch_,
            config.hasSub("solver") ? config.sub("solver")
                                    : Dune::ParameterTree())),
        megSolver_(
            config.hasSub("meg")
                ? MEGSolverFactory<elementType>::template make_meg_solver<
                      degree, typename Traits::VC>(
                      volumeConductorStorage_.get(),
                      Dune::stackobject_to_shared_ptr(solver_->functionSpace()),
                      config.sub("meg"), config.sub("solver"))
                : nullptr),
        solverBackend_(solver_, config.hasSub("solver")
                                    ? config.sub("solver")
                                    : Dune::ParameterTree()),
        eegTransferMatrixSolver_(solver_, config.hasSub("solver")
                                              ? config.sub("solver")
                                              : Dune::ParameterTree()),
        megTransferMatrixSolver_(solver_, megSolver_),
        eegForwardSolver_(solver_)
  {
  }

  virtual void solveEEGForward(
      const typename VolumeConductorInterface<dim>::DipoleType &dipole,
      Function &solution, const Dune::ParameterTree &config,
      DataTree dataTree = DataTree()) override {
    this->solveEEGForward_impl(dipole, solution, config, config_,
                               eegForwardSolver_, *solver_, solverBackend_,
                               dataTree);
    sourceModelPtr_ = eegForwardSolver_.sourceModel();
    if (config.get<bool>("subtract_mean")) {
      subtract_mean(*solver_,
                    solution.cast<typename Traits::DomainDOFVector>());
    }
  }

  virtual std::vector<double>
  solveMEGForward(const Function &eegSolution,
                  Dune::ParameterTree config,
                  DataTree dataTree = DataTree()) override {
    if (!megSolver_) {
      DUNE_THROW(Dune::Exception, "no meg solver created");
    }
    this->featureManager_->check_feature(config);
    megSolver_->bind(eegSolution.cast<typename Traits::DomainDOFVector>());
    std::vector<double> output;
    for (unsigned int i = 0; i < megSolver_->numberOfCoils(); ++i) {
      for (unsigned int j = 0; j < megSolver_->numberOfProjections(i); ++j) {
        std::stringstream name;
        name << "coil_" << i << "_projection_" << j;
        Dune::Timer timer;
        double time_bind = timer.elapsed();
        timer.reset();
        output.push_back(megSolver_->solve(i, j));
        double time_solve = timer.elapsed();
        dataTree.set(name.str() + ".time", time_bind + time_solve);
        dataTree.set(name.str() + ".time_bind", time_bind);
        dataTree.set(name.str() + ".time_solve", time_solve);
      }
    }
    
    if(config.get<bool>("post_process_meg")) {
      if(!sourceModelPtr_) {
        DUNE_THROW(Dune::Exception, "source model not set, but is needed for MEG post processing");
      }

      sourceModelPtr_->postProcessMEG(coils_, projections_, output);
    }
    
    return output;
  }

  virtual std::unique_ptr<Function> makeDomainFunction() const override {
    return std::make_unique<Function>(
        make_domain_dof_vector(*solver_, 0.0));
  }

  virtual void setElectrodes(
      const std::vector<typename VolumeConductorInterface<dim>::CoordinateType>
          &electrodes,
      const Dune::ParameterTree &config) override {
    assert(electrodes.size() > 0);
    electrodeProjection_ =
        ElectrodeProjectionFactory::make_electrode_projection(
            config, volumeConductorStorage_.get()->gridView());
    electrodeProjection_->setElectrodes(electrodes);
    projectedGlobalElectrodes_.clear();
    for (unsigned int i = 0; i < electrodeProjection_->size(); ++i) {
      projectedGlobalElectrodes_.push_back(electrodeProjection_->getProjection(i));
    }
  }

  virtual void setCoilsAndProjections(
      const std::vector<typename VolumeConductorInterface<dim>::CoordinateType>
          &coils,
      const std::vector<
          std::vector<typename VolumeConductorInterface<dim>::CoordinateType>>
          &projections) override {
    if (coils.size() != projections.size()) {
      DUNE_THROW(Dune::Exception,
                 "number of coils ("
                     << coils.size()
                     << ") does not match number of projections ("
                     << projections.size() << ")");
    }
    if (!megSolver_) {
      DUNE_THROW(Dune::Exception, "no meg solver created");
    }
    megSolver_->bind(coils, projections);
    coils_ = coils;
    projections_ = projections;
  }

  virtual std::vector<double>
  evaluateAtElectrodes(const Function &function) const override {
    // create discrete grid function
    using DGF = Dune::PDELab::DiscreteGridFunction<
        typename Traits::Solver::Traits::FunctionSpace::GFS,
        typename Traits::DomainDOFVector>;
    DGF dgf(solver_->functionSpace().getGFS(),
            function.cast<typename Traits::DomainDOFVector>());

    // evalaute discrete grid function at every projection
    std::vector<double> result;
    result.reserve(electrodeProjection_->size());
    for (std::size_t i = 0; i < electrodeProjection_->size(); ++i) {
      const auto &projection = electrodeProjection_->getProjection(i);
      typename DGF::Traits::RangeType y(0.0);
      dgf.evaluate(projection.element, projection.localPosition, y);
      result.push_back(y);
    }
    return result;
  }

  virtual std::unique_ptr<VolumeConductorVTKWriterInterface> volumeConductorVTKWriter(const Dune::ParameterTree& config) const override
  {
    bool visualizeAnisotropy = config.get<bool>("anisotropy.enable", false);
    return std::make_unique<VolumeConductorVTKWriter<typename Traits::Solver>>(*solver_, visualizeAnisotropy);
  }

  virtual std::unique_ptr<DenseMatrix<double>>
  computeEEGTransferMatrix(const Dune::ParameterTree &config,
                           DataTree dataTree = DataTree()) override {
    this->featureManager_->update_features("transfer_matrix");
    return eegTransferMatrixSolver_.solve(solverBackend_, *electrodeProjection_,
                                          config, dataTree);
  }

  virtual std::unique_ptr<DenseMatrix<double>>
  computeMEGTransferMatrix(const Dune::ParameterTree &config,
                           DataTree dataTree = DataTree()) override {
    if (!megSolver_) {
      DUNE_THROW(Dune::Exception, "no meg solver created");
    }
    this->featureManager_->update_features("transfer_matrix");
    return megTransferMatrixSolver_.solve(solverBackend_, config, dataTree);
  }

  virtual std::vector<std::vector<double>> applyEEGTransfer(
      const DenseMatrix<double> &transferMatrix,
      const std::vector<typename VolumeConductorInterface<dim>::DipoleType>
          &dipoles,
      const Dune::ParameterTree &config,
      DataTree dataTree = DataTree()) override {

    return this->template applyEEGTransfer_impl<Traits>(
        transferMatrix, dipoles, config, dataTree, config_, solver_,
        projectedGlobalElectrodes_);
  }

  virtual std::vector<std::vector<double>> applyMEGTransfer(
      const DenseMatrix<double> &transferMatrix,
      const std::vector<typename VolumeConductorInterface<dim>::DipoleType>
          &dipoles,
      const Dune::ParameterTree &config,
      DataTree dataTree = DataTree()) override {
    return this->template applyMEGTransfer_impl<Traits>(
        transferMatrix, dipoles, config, dataTree, config_, solver_, coils_, projections_);
  }
  
  virtual std::vector<std::vector<double>> computeMEGPrimaryField(
    const std::vector<typename VolumeConductorInterface<dim>::DipoleType>& dipoles,
    const Dune::ParameterTree& config) const override
  {
    if(coils_.size() == 0) {
      DUNE_THROW(Dune::Exception, "coils and projections not set");
    }
    return this->computeMEGPrimaryField_impl(dipoles, coils_, projections_, config);
  }

  virtual std::vector<typename VolumeConductorInterface<dim>::CoordinateType>
  getProjectedElectrodes() const override {
    std::vector<typename VolumeConductorInterface<dim>::CoordinateType> coordinates;
    for(size_t i = 0; i < projectedGlobalElectrodes_.size(); ++i) {
      coordinates.push_back(projectedGlobalElectrodes_[i].element.geometry().global(projectedGlobalElectrodes_[i].localPosition));
    }
    return coordinates;
  }

  virtual void statistics(DataTree dataTree) const override {
    auto volumeConductorStatistics =
        computeVolumeConductorStatistics(*(volumeConductorStorage_.get()));
    auto sub = dataTree.sub("volume_conductor");
    for (const auto &dtv : volumeConductorStatistics.domainToVolume) {
      sub.set("volume_label_" + std::to_string(dtv.first), dtv.second);
    }
    for (const auto &itv : volumeConductorStatistics.interfaceToVolume) {
      sub.set("surface_labels_" + std::to_string(itv.first.first) + "_" +
                  std::to_string(itv.first.second),
              itv.second);
    }
  }

private:
  Dune::ParameterTree config_;
  typename Traits::VCStorage volumeConductorStorage_;
  std::shared_ptr<typename Traits::ElementSearch> elementSearch_;
  std::shared_ptr<typename Traits::Solver> solver_;
  std::shared_ptr<
      MEGSolverInterface<typename Traits::VC, typename Traits::DomainDOFVector>>
      megSolver_;
#if HAVE_TBB
  tbb::enumerable_thread_specific<typename Traits::SolverBackend>
      solverBackend_;
#else
  typename Traits::SolverBackend solverBackend_;
#endif
  TransferMatrixSolver<typename Traits::Solver,
                       typename Traits::TransferMatrixRHSFactory>
      eegTransferMatrixSolver_;
  FittedMEGTransferMatrixSolver<typename Traits::Solver>
      megTransferMatrixSolver_;
  EEGForwardSolver<typename Traits::Solver, typename Traits::SourceModelFactory>
      eegForwardSolver_;
  std::unique_ptr<
      duneuro::ElectrodeProjectionInterface<typename Traits::VC::GridView>>
      electrodeProjection_;
  std::vector<typename duneuro::ProjectedElectrode<typename Traits::VC::GridView>>
      projectedGlobalElectrodes_;
  std::vector<typename VolumeConductorInterface<dim>::CoordinateType> coils_;
  std::vector<std::vector<typename VolumeConductorInterface<dim>::CoordinateType>> projections_;
  std::shared_ptr<SourceModelInterface<typename Traits::VC::GridView, double, dim, typename Traits::DomainDOFVector>> sourceModelPtr_;
};

} // namespace duneuro

#endif // FITTED_VOLUME_CONDUCTOR_HH
