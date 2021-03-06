#ifndef DUNEURO_ENTITYSET_VOLUME_CONDUCTOR_HH
#define DUNEURO_ENTITYSET_VOLUME_CONDUCTOR_HH

#include <vector>

#include <dune/grid/common/mcmgmapper.hh>

namespace duneuro
{
  template <class ES>
  class EntitySetVolumeConductor
  {
  public:
    enum { dim = ES::dimension };
    typedef typename ES::ctype ctype;
    typedef typename ES::template Codim<0>::Entity EntityType;
    typedef Dune::FieldMatrix<ctype, dim, dim> TensorType;
    typedef ES EntitySet;
    typedef ES GridView;

    EntitySetVolumeConductor(const ES& entitySet, const std::vector<TensorType>& tensors)
        : entitySet_(entitySet), tensors_(tensors),
          elementMapper_(entitySet_, Dune::mcmgElementLayout())
    {}

    const EntitySet& entitySet() const
    {
      return entitySet_;
    }

    const TensorType& tensor(const EntityType& entity) const
    {
      return tensors_[elementMapper_.index(entity)];
    }

  private:
    ES entitySet_;
    std::vector<TensorType> tensors_;
    Dune::MultipleCodimMultipleGeomTypeMapper<ES> elementMapper_;
  };
}

#endif // DUNEURO_ENTITYSET_VOLUME_CONDUCTOR_HH
