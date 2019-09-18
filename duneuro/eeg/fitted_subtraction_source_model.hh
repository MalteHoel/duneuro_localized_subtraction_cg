#ifndef DUNEURO_FITTED_SUBTRACTION_SOURCE_MODEL_HH
#define DUNEURO_FITTED_SUBTRACTION_SOURCE_MODEL_HH

#include <dune/common/parametertree.hh>

#include <dune/pdelab/backend/interface.hh>
#include <dune/pdelab/boilerplate/pdelab.hh>

#include <duneuro/common/edge_norm_provider.hh>
#include <duneuro/common/penalty_flux_weighting.hh>
#include <duneuro/eeg/source_model_interface.hh>
#include <duneuro/eeg/subtraction_dg_default_parameter.hh>
#include <duneuro/eeg/subtraction_dg_operator.hh>

namespace duneuro
{
  template <class VC, class FS, class V, SubtractionContinuityType continuityType>
  class FittedSubtractionSourceModel
      : public SourceModelBase<typename FS::GFS::Traits::GridViewType, V>
  {
  public:
    using BaseT = SourceModelBase<typename FS::GFS::Traits::GridViewType, V>;
    enum { dim = VC::dim };
    using Problem = SubtractionDGDefaultParameter<typename FS::GFS::Traits::GridViewType,
                                                  typename V::field_type, VC>;
    using EdgeNormProvider = MultiEdgeNormProvider;
    using PenaltyFluxWeighting = FittedDynamicPenaltyFluxWeights;
    using LOP = SubtractionDG<Problem, EdgeNormProvider, PenaltyFluxWeighting, continuityType>;
    using DOF = typename FS::DOF;
    using AS = Dune::PDELab::GalerkinGlobalAssembler<FS, LOP, Dune::SolverCategory::sequential>;
    using ElementType = typename BaseT::ElementType;
    using CoordinateType = typename BaseT::CoordinateType;
    using VectorType = typename BaseT::VectorType;
    using SearchType = typename BaseT::SearchType;

    FittedSubtractionSourceModel(std::shared_ptr<const VC> volumeConductor, const FS& fs,
                                 std::shared_ptr<const SearchType> search,
                                 const Dune::ParameterTree& config,
                                 const Dune::ParameterTree& solverConfig)
        : BaseT(search)
        , problem_(volumeConductor->gridView(), volumeConductor)
        , edgeNormProvider_(solverConfig.get<std::string>("edge_norm_type", "houston"), 1.0)
        , weighting_(solverConfig.get<std::string>("weights", "tensorOnly"))
        , lop_(problem_, weighting_, config.get<unsigned int>("intorderadd"),
               config.get<unsigned int>("intorderadd_lb"))
        , x_(fs.getGFS(), 0.0)
        , res_(fs.getGFS(), 0.0)
        , interp_(fs.getGFS(), 0.0)
        , assembler_(fs, lop_, 1)
    {
    }

    virtual void bind(const typename BaseT::DipoleType& dipole,
                      DataTree dataTree = DataTree()) override
    {
      BaseT::bind(dipole, dataTree);
      problem_.bind(this->dipoleElement(), this->localDipolePosition(), this->dipole().moment());
    }

    virtual void assembleRightHandSide(VectorType& vector) const override
    {
      x_ = 0.0;
      assembler_->residual(x_, vector);
      vector *= -1.0;
    }

    virtual void postProcessSolution(VectorType& vector) const override
    {
      interp_ = 0.0;
      Dune::PDELab::interpolate(problem_.get_u_infty(), assembler_->trialGridFunctionSpace(),
                                interp_);
      vector += interp_;
    }

    virtual void
    postProcessSolution(const std::vector<CoordinateType>& electrodes,
                        std::vector<typename VectorType::field_type>& vector) const override
    {
      assert(electrodes.size() == vector.size());
      Dune::FieldVector<typename Problem::Traits::RangeFieldType, 1> result;
      for (unsigned int i = 0; i < electrodes.size(); ++i) {
        problem_.get_u_infty().evaluateGlobal(electrodes[i], result);
        vector[i] += result;
      }
    }

  private:
    Problem problem_;
    EdgeNormProvider edgeNormProvider_;
    PenaltyFluxWeighting weighting_;
    LOP lop_;
    mutable DOF x_;
    mutable DOF res_;
    mutable DOF interp_;
    mutable AS assembler_;
  };
}

#endif // DUNEURO_FITTED_SUBTRACTION_SOURCE_MODEL_HH