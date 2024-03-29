#ifndef DUNEURO_P1_GRADIENT_FEM_HH
#define DUNEURO_P1_GRADIENT_FEM_HH

#include <type_traits>

#include <dune/pdelab/finiteelementmap/finiteelementmap.hh>

#include <duneuro/common/p1gradient2d.hh>
#include <duneuro/common/p1gradient3d.hh>

namespace duneuro
{
  template <typename GV, typename D, typename R, typename = void>
  class P1GradientLocalFiniteElementMap;

  template <typename GV, typename D, typename R>
  class P1GradientLocalFiniteElementMap<GV, D, R,
                                        typename std::enable_if<GV::dimension == 2, void>::type>
      : public Dune::PDELab::SimpleLocalFiniteElementMap<P1Gradient2DLocalFiniteElement<D, R>, GV::dimension>
  {
  public:
    static constexpr bool fixedSize()
    {
      return true;
    }

    static constexpr bool hasDOFs(int codim)
    {
      return codim == 0;
    }

    static constexpr std::size_t size(Dune::GeometryType gt)
    {
      if (gt == Dune::GeometryTypes::simplex(2))
        return 2;
      else
        return 0;
    }

    static constexpr std::size_t maxLocalSize()
    {
      return 2;
    }

    //! return order of polynomial basis
    static constexpr std::size_t order()
    {
      return 1;
    }
  };

  template <typename GV, typename D, typename R>
  class P1GradientLocalFiniteElementMap<GV, D, R,
                                        typename std::enable_if<GV::dimension == 3, void>::type>
      : public Dune::PDELab::SimpleLocalFiniteElementMap<P1Gradient3DLocalFiniteElement<D, R>, GV::dimension>
  {
  public:
    static constexpr bool fixedSize()
    {
      return true;
    }

    static constexpr bool hasDOFs(int codim)
    {
      return codim == 0;
    }

    static constexpr std::size_t size(Dune::GeometryType gt)
    {
      if (gt == Dune::GeometryTypes::simplex(3))
        return 3;
      else
        return 0;
    }

    static constexpr std::size_t maxLocalSize()
    {
      return 3;
    }

    //! return order of polynomial basis
    static constexpr std::size_t order()
    {
      return 1;
    }
  };
}

#endif // DUNEURO_P1_GRADIENT_FEM_HH
