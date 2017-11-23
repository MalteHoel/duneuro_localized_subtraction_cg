#ifndef DUNEURO_CUTFEM_SOLVER_HH
#define DUNEURO_CUTFEM_SOLVER_HH

#include <dune/udg/pdelab/cutfemmultiphaseoperator.hh>
#include <dune/udg/pdelab/multiphaseoperator.hh>
#include <dune/udg/pdelab/operator.hh>
#include <dune/udg/pdelab/subtriangulation.hh>

#include <dune/pdelab/backend/istl.hh>
#include <dune/pdelab/stationary/linearproblem.hh>

#include <duneuro/common/convection_diffusion_dg_operator.hh>
#include <duneuro/common/convection_diffusion_udg_default_parameter.hh>
#include <duneuro/common/cutfem_gridoperator.hh>
#include <duneuro/common/cutfem_multi_phase_space.hh>
#include <duneuro/common/edge_norm_provider.hh>
#include <duneuro/common/linear_problem_solver.hh>
#include <duneuro/common/random.hh>

namespace duneuro
{
  template <class ST, int comps, int degree, class P, class DF, class RF, class JF>
  struct CutFEMSolverTraits {
    using SubTriangulation = ST;
    using FundamentalGridView = typename ST::BaseT::GridView;
    static const int dimension = FundamentalGridView::dimension;
    static const int compartments = comps;
    using Problem = P;
    using FunctionSpace = CutFEMMultiPhaseSpace<FundamentalGridView, RF, degree, compartments>;
    using DomainField = DF;
    using RangeField = RF;
    using DomainDOFVector = Dune::PDELab::Backend::Vector<typename FunctionSpace::GFS, DF>;
    using RangeDOFVector = Dune::PDELab::Backend::Vector<typename FunctionSpace::GFS, RF>;
    using EdgeNormProvider = MultiEdgeNormProvider;
    using LocalOperator = ConvectionDiffusion_DG_LocalOperator<Problem, EdgeNormProvider>;
    using WrappedLocalOperator = Dune::UDG::CutFEMMultiPhaseLocalOperatorWrapper<LocalOperator>;
    // using WrappedLocalOperator = Dune::UDG::MultiPhaseLocalOperatorWrapper<LocalOperator>;
    using UnfittedSubTriangulation = Dune::PDELab::UnfittedSubTriangulation<FundamentalGridView>;
    using MatrixBackend = Dune::PDELab::istl::BCRSMatrixBackend<>;
    using RawGridOperator =
        Dune::UDG::UDGGridOperator<typename FunctionSpace::GFS, typename FunctionSpace::GFS,
                                   WrappedLocalOperator, MatrixBackend, DF, RF, JF,
                                   UnfittedSubTriangulation>;
    using GridOperator = CutFEMGridOperator<RawGridOperator, ST, EdgeNormProvider>;
    using SolverBackend = Dune::PDELab::ISTLBackend_SEQ_CG_AMG_SSOR<GridOperator>;
    using LinearSolver = LinearProblemSolver<GridOperator, DomainDOFVector, RangeDOFVector>;
  };

  template <class ST, int compartments, int degree,
            class P = ConvectionDiffusion_UDG_DefaultParameter<typename ST::BaseT::GridView>,
            class DF = double, class RF = double, class JF = double>
  class CutFEMSolver
  {
  public:
    using Traits = CutFEMSolverTraits<ST, compartments, degree, P, DF, RF, JF>;

    CutFEMSolver(std::shared_ptr<typename Traits::SubTriangulation> subTriangulation,
                 const Dune::ParameterTree& config)
        : CutFEMSolver(subTriangulation, std::make_shared<typename Traits::Problem>(
                                             config.get<std::vector<double>>("conductivities")),
                       config)
    {
    }

    CutFEMSolver(std::shared_ptr<typename Traits::SubTriangulation> subTriangulation,
                 std::shared_ptr<typename Traits::Problem> problem,
                 const Dune::ParameterTree& config)
        : subTriangulation_(subTriangulation)
        , problem_(problem)
        , functionSpace_(subTriangulation_->gridView(), subTriangulation_)
        , edgeNormProvider_(config.get<std::string>("edge_norm_type"), 1.0)
        , localOperator_(*problem_, edgeNormProvider_, ConvectionDiffusion_DG_Scheme::fromString(
                                                           config.get<std::string>("scheme")),
                         ConvectionDiffusion_DG_Weights::weightsOn, config.get<RF>("penalty"))
        , wrappedLocalOperator_(localOperator_)
        , unfittedSubTriangulation_(subTriangulation_->gridView(), *subTriangulation_)
        , rawGridOperator_(functionSpace_.getGFS(), functionSpace_.getGFS(),
                           unfittedSubTriangulation_, wrappedLocalOperator_,
                           typename Traits::MatrixBackend(2 * Traits::dimension + 1))
        , gridOperator_(rawGridOperator_, subTriangulation, edgeNormProvider_, config)
        , linearSolver_(gridOperator_, config)
    {
    }

    template <class SolverBackend>
    void solve(SolverBackend& solverBackend, const typename Traits::RangeDOFVector& rightHandSide,
               typename Traits::DomainDOFVector& solution, const Dune::ParameterTree& config,
               DataTree dataTree = DataTree())
    {
      Dune::Timer timer;
      randomize_uniform(Dune::PDELab::Backend::native(solution), DF(-1.0), DF(1.0));
      linearSolver_.apply(solverBackend, solution, rightHandSide, config, dataTree);
      dataTree.set("time", timer.elapsed());
    }

    template <class SolverBackend>
    void solve(SolverBackend& solverBackend, typename Traits::DomainDOFVector& solution,
               const Dune::ParameterTree& config, DataTree dataTree = DataTree())
    {
      Dune::Timer timer;
      randomize_uniform(Dune::PDELab::Backend::native(solution), DF(-1.0), DF(1.0));
      linearSolver_.apply(solverBackend, solution, config, dataTree);
      dataTree.set("time", timer.elapsed());
    }

    const typename Traits::FunctionSpace& functionSpace() const
    {
      return functionSpace_;
    }

    const typename Traits::SubTriangulation& subTriangulation() const
    {
      return *subTriangulation_;
    }

    typename Traits::Problem& problem()
    {
      return *problem_;
    }

  private:
    std::shared_ptr<typename Traits::SubTriangulation> subTriangulation_;
    std::shared_ptr<typename Traits::Problem> problem_;
    typename Traits::FunctionSpace functionSpace_;
    typename Traits::EdgeNormProvider edgeNormProvider_;
    typename Traits::LocalOperator localOperator_;
    typename Traits::WrappedLocalOperator wrappedLocalOperator_;
    typename Traits::UnfittedSubTriangulation unfittedSubTriangulation_;
    typename Traits::RawGridOperator rawGridOperator_;
    typename Traits::GridOperator gridOperator_;
    typename Traits::LinearSolver linearSolver_;
  };
}

#endif // DUNEURO_CUTFEM_SOLVER_HH
