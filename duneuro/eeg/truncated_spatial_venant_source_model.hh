#ifndef DUNEURO_TRUNCATED_SPATIAL_VENANT_SOURCE_MODEL_HH
#define DUNEURO_TRUNCATED_SPATIAL_VENANT_SOURCE_MODEL_HH

#include <Eigen/Dense>
#include <array>
#include <unordered_map>
#include <vector>

#include <dune/common/fvector.hh>
#include <dune/common/parametertree.hh>

#include <dune/grid/utility/multiindex.hh>

#include <dune/pdelab/backend/interface.hh>
#include <dune/pdelab/gridfunctionspace/entityindexcache.hh>

#include <duneuro/common/dipole.hh>
#include <duneuro/common/element_patch.hh>
#include <duneuro/common/sparse_vector_container.hh>
#include <duneuro/eeg/source_model_interface.hh>

namespace duneuro
{
  template <class VC, class GFS, class V>
  class TruncatedSpatialVenantSourceModel
      : public SourceModelBase<typename GFS::Traits::GridViewType, V>
  {
  public:
    using BaseT = SourceModelBase<typename GFS::Traits::GridView, V>;
    using DipoleType = typename BaseT::DipoleType;
    using CoordinateType = typename BaseT::CoordinateType;
    using VectorType = typename BaseT::VectorType;
    using GV = typename GFS::Traits::GridViewType;
    enum { dim = GV::dimension };
    using Real = typename GV::ctype;
    using Elemeht = typename GV::template Codim<0>::Entity;
    using Vertex = typename GV::template Codim<dim>::Entity;
    using SearchType = typename BaseT::SearchType;
    using VertexMapper = Dune::SingleCodimSingleGeomTypeMapper<GV, dim>;
    using VertexIndex = typename VertexMapper::Index;
    using LFS = Dune::PDELab::LocalFunctionSpace<GFS>;
    using Cache = Dune::PDELab::LFSIndexCache<LFS>;
    using FESwitch = Dune::FiniteElementInterfaceSwitch<typename LFS::Traits::FiniteElementType>;
    using BasisSwitch = Dune::BasisInterfaceSwitch<typename FESwitch::Basis>;
    using RangeType = typename BasisSwitch::Range;
    using DOFIndex = typename GFS::Ordering::Traits::DOFIndex;

    TruncatedSpatialVenantSourceModel(std::shared_ptr<const VC> volumeConductor, const GFS& gfs,
                                      std::shared_ptr<const SearchType> search,
                                      const Dune::ParameterTree& params)
        : BaseT(search)
        , volumeConductor_(volumeConductor)
        , elementNeighborhoodMap_(volumeConductor_->elementNeighborhoodMap())
        , gfs_(gfs)
        , vertexMapper_(gfs_.gridView())
        , numberOfMoments_(params.get<unsigned int>("numberOfMoments"))
        , referenceLength_(params.get<Real>("referenceLength"))
        , weightingExponent_(params.get<unsigned int>("weightingExponent"))
        , relaxationFactor_(params.get<Real>("relaxationFactor"))
        , mixedMoments_(params.get<bool>("mixedMoments"))
        , config_(params)
    {
      assert(weightingExponent_ < numberOfMoments_);
    }

    virtual void bind(const typename BaseT::DipoleType& dipole,
                      DataTree dataTree = DataTree()) override
    {
      BaseT::bind(dipole, dataTree);

      patch_ = make_element_patch(volumeConductor_, elementNeighborhoodMap_, this->elementSearch(),
                                  dipole.position(), config_);

      dofToLinear_ = extractPatchDOFIndices();

      interpolatedDOFs_ = solveMomentSystem(dipole);
    }

    virtual void assembleRightHandSide(VectorType& vector) const
    {
      LFS lfs(gfs_);
      Cache cache(lfs);

      for (const auto& element : patch_->elements()) {
        lfs.bind(element);
        cache.update();
        std::vector<RangeType> phi(lfs.size());
        const auto& geo = element.geometry();
        const auto intorder = 2 * FESwitch::basis(lfs.finiteElement()).order()
                              + config_.get<unsigned int>("intorderadd");
        const auto& rule = Dune::QuadratureRules<Real, dim>::rule(geo.type(), intorder);
        for (const auto& qp : rule) {
          FESwitch::basis(lfs.finiteElement()).evaluateFunction(qp.position(), phi);
          RangeType sourceTerm(0.0);
          for (unsigned int i = 0; i < lfs.size(); ++i) {
            sourceTerm += interpolatedDOFs_[dofToLinear_.find(cache.dofIndex(i))->second] * phi[i];
          }
          auto factor = qp.weight() * geo.integrationElement(qp.position()) * sourceTerm;
          for (unsigned int i = 0; i < lfs.size(); ++i) {
            vector[cache.containerIndex(i)] += factor * phi[i];
          }
        }
      }
    }

  private:
    std::shared_ptr<const VC> volumeConductor_;
    std::shared_ptr<ElementNeighborhoodMap<GV>> elementNeighborhoodMap_;
    const GFS& gfs_;
    VertexMapper vertexMapper_;
    const unsigned int numberOfMoments_;
    const Real referenceLength_;
    const unsigned int weightingExponent_;
    const Real relaxationFactor_;
    const bool mixedMoments_;
    Dune::ParameterTree config_;
    std::vector<Real> interpolatedDOFs_;
    std::unordered_map<DOFIndex, std::size_t> dofToLinear_;
    std::unique_ptr<ElementPatch<GV>> patch_;

    /**
     * \brief assemble and solve the moment system for the given dipole and dofs
     */
    std::vector<Real> solveMomentSystem(const DipoleType& dipole) const
    {
      const auto& multiIndices = createMomentExponents<dim>(numberOfMoments_, mixedMoments_);
      Eigen::MatrixXd momentMatrix = assembleMomentMatrix(multiIndices, dipole.position());
      Eigen::VectorXd rightHandSide = assembleMomentVector(multiIndices, dipole.moment());
      Eigen::MatrixXd weightMatrix = assembleWeightMatrix(multiIndices, dipole.position());
      Eigen::MatrixXd systemMatrix = momentMatrix.transpose() * momentMatrix
                                     + relaxationFactor_ * weightMatrix.transpose() * weightMatrix;
      Eigen::VectorXd systemRHS = momentMatrix.transpose() * rightHandSide;
      Eigen::VectorXd solution = systemMatrix.colPivHouseholderQr().solve(systemRHS);
      std::vector<Real> result;
      result.reserve(solution.size());
      for (unsigned int i = 0; i < solution.size(); ++i) {
        result.push_back(solution[i]);
      }
      return result;
    }

    /**
     * \brief commpute the moment vector of the source term
     */
    Eigen::VectorXd
    assembleMomentVector(const std::vector<std::array<unsigned int, dim>>& multiIndices,
                         const CoordinateType& moment) const
    {
      Eigen::VectorXd result = Eigen::VectorXd::Zero(multiIndices.size());
      for (unsigned int i = 0; i < multiIndices.size(); ++i) {
        if (oneNorm(multiIndices[i]) == 1) {
          for (unsigned int j = 0; j < dim; ++j) {
            if (multiIndices[i][j] > 0) {
              result[i] = moment[j] / referenceLength_;
              break;
            }
          }
        }
      }
      return result;
    }

    /**
     * \brief assemble the weight matrix
     *
     * The resulting matrix is to be used in the regularizer and weights the dofs
     * according to their distance to the given position. If the weighting exponent
     * has been set to 0, the identity will be returned.
     */
    Eigen::MatrixXd
    assembleWeightMatrix(const std::vector<std::array<unsigned int, dim>>& multiIndices,
                         const CoordinateType& position) const
    {
      Eigen::MatrixXd result = Eigen::MatrixXd::Zero(dofToLinear_.size(), dofToLinear_.size());
      LFS lfs(gfs_);
      Cache cache(lfs);
      for (const auto& element : patch_->elements()) {
        const auto& geo = element.geometry();
        lfs.bind(element);
        cache.update();
        std::vector<RangeType> phi(lfs.size());
        const auto order = FESwitch::basis(lfs.finiteElement()).order();
        unsigned int intorder =
            order + weightingExponent_ + config_.get<unsigned int>("intorderadd");
        const auto& rule = Dune::QuadratureRules<Real, dim>::rule(geo.type(), intorder);
        for (const auto& qp : rule) {
          FESwitch::basis(lfs.finiteElement()).evaluateFunction(qp.position(), phi);
          auto diff = geo.global(qp.position()) - position;
          diff /= referenceLength_;
          auto factor = qp.weight() * geo.integrationElement(qp.position())
                        * ipow(diff.two_norm(), weightingExponent_);
          for (unsigned int i = 0; i < lfs.size(); ++i) {
            auto linear = dofToLinear_.find(cache.dofIndex(i))->second;
            result(linear, linear) += factor * phi[i];
          }
        }
      }
      return result;
    }

    /**
     * \brief assemble the matrix of centered moments
     *
     * Compute the matrix of centered moments around the given position on the patch
     * for the basis function that are part of the given dof vector.
     */
    Eigen::MatrixXd
    assembleMomentMatrix(const std::vector<std::array<unsigned int, dim>>& multiIndices,
                         const CoordinateType& position) const
    {
      Eigen::MatrixXd result = Eigen::MatrixXd::Zero(multiIndices.size(), dofToLinear_.size());
      LFS lfs(gfs_);
      Cache cache(lfs);
      for (const auto& element : patch_->elements()) {
        const auto& geo = element.geometry();
        lfs.bind(element);
        cache.update();
        std::vector<RangeType> phi(lfs.size());
        const auto order = FESwitch::basis(lfs.finiteElement()).order();
        unsigned int intorder =
            order + numberOfMoments_ - 1 + config_.get<unsigned int>("intorderadd");
        const auto& rule = Dune::QuadratureRules<Real, dim>::rule(geo.type(), intorder);
        for (const auto& qp : rule) {
          FESwitch::basis(lfs.finiteElement()).evaluateFunction(qp.position(), phi);
          auto diff = geo.global(qp.position()) - position;
          diff /= referenceLength_;
          std::vector<Real> values;
          for (const auto& mi : multiIndices) {
            values.push_back(pow(diff, mi));
          }
          auto factor = qp.weight() * geo.integrationElement(qp.position());
          for (unsigned int i = 0; i < lfs.size(); ++i) {
            for (unsigned int j = 0; j < values.size(); ++j) {
              result(j, dofToLinear_.find(cache.dofIndex(i))->second) +=
                  factor * phi[i] * values[j];
            }
          }
        }
      }
      return result;
    }

    /**
     * \brief extract the indices of all vertices of the given patch
     */
    std::unordered_map<DOFIndex, std::size_t> extractPatchDOFIndices() const
    {
      std::unordered_map<DOFIndex, std::size_t> result;
      LFS lfs(gfs_);
      Cache cache(lfs);
      for (const auto& element : patch_->elements()) {
        lfs.bind(element);
        cache.update();
        for (unsigned int i = 0; i < lfs.size(); ++i) {
          result.insert({cache.dofIndex(i), result.size()});
        }
      }
      return result;
    }
  };
}

#endif // DUNEURO_TRUNCATED_SPATIAL_VENANT_SOURCE_MODEL_HH
