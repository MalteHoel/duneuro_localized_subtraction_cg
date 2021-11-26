#ifndef DUNEURO_LOCALIZED_SUBTRACTION_SOURCE_MODEL_HH
#define DUNEURO_LOCALIZED_SUBTRACTION_SOURCE_MODEL_HH

#include <dune/common/parametertree.hh>

#include <dune/grid/common/rangegenerators.hh>

#include <dune/pdelab/backend/interface.hh>
#include <dune/pdelab/boilerplate/pdelab.hh>
#include <dune/pdelab/function/discretegridviewfunction.hh>             // include for the DiscreteGridViewFunction class


#include <duneuro/common/convection_diffusion_dg_operator.hh>
#include <duneuro/common/edge_norm_provider.hh>
#include <duneuro/common/element_patch.hh>
#include <duneuro/common/entityset_volume_conductor.hh>
#include <duneuro/common/element_patch_assembler.hh>
#include <duneuro/common/logged_timer.hh>
#include <duneuro/common/penalty_flux_weighting.hh>
#include <duneuro/common/sub_function_space.hh>
#include <duneuro/common/subset_entityset.hh>
#include <duneuro/eeg/source_model_interface.hh>
#include <duneuro/eeg/subtraction_dg_default_parameter.hh>
#include <duneuro/eeg/subtraction_dg_operator.hh>
#include <duneuro/eeg/localized_subtraction_dg_local_operator.hh>
#include <duneuro/eeg/localized_subtraction_cg_local_operator.hh>
#include <duneuro/common/flags.hh>

namespace duneuro
{
  template <class VC, class FS, class V, ContinuityType continuityType>
  class LocalizedSubtractionSourceModel
      : public SourceModelBase<typename FS::GFS::Traits::GridViewType, V>
  {
  public:
    using BaseT = SourceModelBase<typename FS::GFS::Traits::GridViewType, V>;
    enum { dim = VC::dim };
    using ElementType = typename BaseT::ElementType;
    using CoordinateType = typename BaseT::CoordinateType;
    using VectorType = typename BaseT::VectorType;
    using SearchType = typename BaseT::SearchType;
    using HostGridView = typename VC::GridView;
    using SubEntitySet = SubSetEntitySet<HostGridView>;
    using SubVolumeConductor = EntitySetVolumeConductor<SubEntitySet>;
    using SUBFS = SubFunctionSpace<FS, SubVolumeConductor>;
    using Problem =
        SubtractionDGDefaultParameter<SubEntitySet, typename V::field_type, SubVolumeConductor>;
    using EdgeNormProvider = MultiEdgeNormProvider;
    using PenaltyFluxWeighting = FittedDynamicPenaltyFluxWeights;
    using LOP = SubtractionDG<Problem, EdgeNormProvider, PenaltyFluxWeighting>;
    using DOF = typename SUBFS::DOF;
    using AS = Dune::PDELab::GalerkinGlobalAssembler<SUBFS, LOP, Dune::SolverCategory::sequential>;
    using SubLFS = Dune::PDELab::LocalFunctionSpace<typename SUBFS::GFS>;
    using SubLFSCache = Dune::PDELab::LFSIndexCache<SubLFS>;
    using HostLFS = Dune::PDELab::LocalFunctionSpace<typename FS::GFS>;
    using HostLFSCache = Dune::PDELab::LFSIndexCache<HostLFS>;
    using HostProblem = SubtractionDGDefaultParameter<HostGridView, typename V::field_type, VC>;
    using DOFVector = Dune::PDELab::Backend::Vector<typename FS::GFS, typename FS::NT>;
    using DiscreteGridFunction = typename Dune::PDELab::DiscreteGridViewFunction<typename FS::GFS, DOFVector>;

    LocalizedSubtractionSourceModel(std::shared_ptr<const VC> volumeConductor,
                                    std::shared_ptr<const FS> fs,
                                    std::shared_ptr<const SearchType> search,
                                    const Dune::ParameterTree& config,
                                    const Dune::ParameterTree& solverConfig)
        : BaseT(search)
        , volumeConductor_(volumeConductor)
        , functionSpace_(fs)
        , elementNeighborhoodMap_(std::make_shared<ElementNeighborhoodMap<typename VC::GridView>>(
              volumeConductor_->gridView()))
        , config_(config)
        , patchAssembler_(volumeConductor_,functionSpace_,search,config_)
        // parameters for LOP
        , edgeNormProvider_(solverConfig.get<std::string>("edge_norm_type"), 1.0)
        , weighting_(solverConfig.get<std::string>("weights"))
        , intorderadd_(config.get<unsigned int>("intorderadd"))
        , intorderadd_lb_(config.get<unsigned int>("intorderadd_lb"))
        , penalty_(solverConfig.get<double>("penalty"))
        , chiFunctionPtr_(nullptr)
    {
    }

    virtual void bind(const typename BaseT::DipoleType& dipole,
                      DataTree dataTree = DataTree()) override
    {
      LoggedTimer timer(dataTree);
      BaseT::bind(dipole, dataTree);
      timer.lap("bind_base");

      // build patch
      patchAssembler_.bind(dipole.position(), dataTree);
      timer.lap("bind_patch_assembler");
    
      // setup of problem parameters (everything related to the evaluation of u_infinity, its gradient and sigma_infinity)
      hostProblem_ = std::make_shared<HostProblem>(volumeConductor_->gridView(), volumeConductor_);
      hostProblem_->bind(this->dipoleElement(), this->localDipolePosition(),
                         this->dipole().moment());


      dataTree.set("elements", patchAssembler_.patchElements().size());

      if constexpr(continuityType == ContinuityType::discontinuous)
      {
        SubEntitySet subEntitySet(volumeConductor_->gridView(), patchAssembler_.patchElements());
        timer.lap("create_sub_entity_set");

        // extract conductivity tensors to create a local volume conductor
        Dune::MultipleCodimMultipleGeomTypeMapper<SubEntitySet>
            mapper(subEntitySet, Dune::mcmgElementLayout());
        std::vector<typename VC::TensorType> tensors(mapper.size());
        for (const auto& subElement : patchAssembler_.patchElements()) {
          tensors[mapper.index(subElement)] = volumeConductor_->tensor(subElement);
        }
        timer.lap("extract_sub_tensors");

        // create sub grid volume conductor
        subVolumeConductor_ = std::make_shared<SubVolumeConductor>(subEntitySet, tensors);
        timer.lap("sub_volume_conductor");

        problem_ = std::make_shared<Problem>(subVolumeConductor_->entitySet(), subVolumeConductor_);
        problem_->bind(this->dipoleElement(), this->localDipolePosition(), this->dipole().moment());
        lop_ = std::make_shared<LOP>(*problem_, weighting_, config_.get<unsigned int>("intorderadd"),
                                     config_.get<unsigned int>("intorderadd_lb"));
        subFS_ = std::make_shared<SUBFS>(subVolumeConductor_);
        dataTree.set("sub_dofs", subFS_->getGFS().size());
        x_ = std::make_shared<DOF>(subFS_->getGFS(), 0.0);
        r_ = std::make_shared<DOF>(subFS_->getGFS(), 0.0);
        assembler_ = std::make_shared<AS>(*subFS_, *lop_, 1);
        timer.lap("sub_problem");
        timer.stop("bind_accumulated");
        // note: maybe invert normal in boundary condition of subtraction operator??
      }
      else if(continuityType == ContinuityType::continuous)
      {
        // create chi function
        HostLFS lfs(functionSpace_->getGFS());
        HostLFSCache indexMapper(lfs);

        // we first create the underlying vector containing the coefficients of chi when written in the FEM basis
        chiBasisCoefficientsPtr_ = std::make_shared<DOFVector>(functionSpace_->getGFS(), 0.0);

        // we now iterate over the inner region and set all coefficients corresponding to DOFs of elements contained in the inner region to 1.0
        for(const auto& element : patchAssembler_.patchElements()) {
          // bind local finite element space
          lfs.bind(element);
          indexMapper.update();

          for(size_t i = 0; i < indexMapper.size(); ++i) {
            auto index = indexMapper.containerIndex(i);
            (*chiBasisCoefficientsPtr_)[index] = 1.0;
          } // end inner for loop
        } //end outer for loop

        // we can now wrap chi into a grid function
        chiFunctionPtr_ = std::make_shared<DiscreteGridFunction>(functionSpace_->getGFS(), *chiBasisCoefficientsPtr_);
      } // end if
    } // end bind

    virtual void assembleRightHandSide(VectorType& vector) const override
    {
      if constexpr(continuityType == ContinuityType::discontinuous)
      {
        assembleLocalDefaultSubtraction(vector);
        using LOP = LocalizedSubtractionDGLocalOperator<HostProblem, EdgeNormProvider, PenaltyFluxWeighting>;
        LOP lop2(*hostProblem_, edgeNormProvider_, weighting_, penalty_, intorderadd_lb_);
        patchAssembler_.assemblePatchBoundary(vector, lop2);
      }
      else if(continuityType == ContinuityType::continuous)
      {
        using LOP = LocalizedSubtractionCGLocalOperator<VC, DiscreteGridFunction, HostProblem>;
        LOP cg_local_operator(volumeConductor_, chiFunctionPtr_, *hostProblem_, intorderadd_, intorderadd_lb_);
        patchAssembler_.assemblePatchVolume(vector, cg_local_operator);
        patchAssembler_.assemblePatchBoundary(vector, cg_local_operator);
        patchAssembler_.assembleTransitionVolume(vector, cg_local_operator);
      }
    }

    virtual void postProcessSolution(VectorType& vector) const override
    {
      if constexpr(continuityType == ContinuityType::discontinuous)
      {
        *x_ = 0.0;
        Dune::PDELab::interpolate(problem_->get_u_infty(), (*assembler_)->trialGridFunctionSpace(),
                                  *x_);

        SubLFS sublfs(subFS_->getGFS());
        SubLFSCache subcache(sublfs);
        HostLFS hostlfs(functionSpace_->getGFS());
        HostLFSCache hostcache(hostlfs);
        for (const auto& e : Dune::elements(subVolumeConductor_->entitySet())) {
          sublfs.bind(e);
          subcache.update();
          hostlfs.bind(e);
          hostcache.update();
          for (unsigned int i = 0; i < hostcache.size(); ++i) {
            vector[hostcache.containerIndex(i)] += (*x_)[subcache.containerIndex(i)];
          }
        }
      }
      else if(continuityType == ContinuityType::continuous)
      {
        //TODO
      }
    }

    virtual void
    postProcessSolution(const std::vector<CoordinateType>& electrodes,
                        std::vector<typename VectorType::field_type>& vector) const override
    {
      // note: need to check if electrode is within the patch
      // currently assume that the patch does not touch the boundary
    }

  private:
    std::shared_ptr<const VC> volumeConductor_;
    std::shared_ptr<const FS> functionSpace_;
    std::shared_ptr<ElementNeighborhoodMap<typename VC::GridView>> elementNeighborhoodMap_;
    std::shared_ptr<SubVolumeConductor> subVolumeConductor_;
    std::shared_ptr<Problem> problem_;
    std::shared_ptr<HostProblem> hostProblem_;
    std::shared_ptr<LOP> lop_;
    std::shared_ptr<SUBFS> subFS_;
    std::shared_ptr<AS> assembler_;
    std::shared_ptr<DOF> x_;
    std::shared_ptr<DOF> r_;
    Dune::ParameterTree config_;
    ElementPatchAssembler<VC, FS> patchAssembler_;
    EdgeNormProvider edgeNormProvider_;
    PenaltyFluxWeighting weighting_;
    unsigned int intorderadd_;
    unsigned int intorderadd_lb_;
    double penalty_;
    std::shared_ptr<DOFVector> chiBasisCoefficientsPtr_;
    std::shared_ptr<DiscreteGridFunction> chiFunctionPtr_;

    void assembleLocalDefaultSubtraction(VectorType& vector) const
    {
      *x_ = 0.0;
      *r_ = 0.0;
      (*assembler_)->residual(*x_, *r_);
      *r_ *= -1.;

      SubLFS sublfs(subFS_->getGFS());
      SubLFSCache subcache(sublfs);
      HostLFS hostlfs(functionSpace_->getGFS());
      HostLFSCache hostcache(hostlfs);
      for (const auto& e : Dune::elements(subVolumeConductor_->entitySet())) {
        sublfs.bind(e);
        subcache.update();
        hostlfs.bind(e);
        hostcache.update();
        for (unsigned int i = 0; i < hostcache.size(); ++i) {
          vector[hostcache.containerIndex(i)] = (*r_)[subcache.containerIndex(i)];
        }
      }
    }
  };
}

#endif // DUNEURO_LOCALIZED_SUBTRACTION_SOURCE_MODEL_HH
