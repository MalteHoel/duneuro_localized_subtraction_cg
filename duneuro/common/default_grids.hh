#ifndef DUNEURO_DEFAULTGRIDS_HH
#define DUNEURO_DEFAULTGRIDS_HH

#if HAVE_DUNE_UGGRID
#include <dune/grid/uggrid.hh>
#endif

#include <duneuro/common/flags.hh>

namespace duneuro
{
  template <int dim, ElementType et>
  struct DefaultGrid;

  template <int d>
  struct DefaultGrid<d, ElementType::hexahedron> {
    enum { dim = d };
#if HAVE_DUNE_UGGRID
    using GridType = Dune::UGGrid<dim>;
#else
#error "no grid manager found. provide ugggrid"
#endif
  };

  template <int d>
  struct DefaultGrid<d, ElementType::tetrahedron> {
    enum { dim = d };
#if HAVE_DUNE_UGGRID
    using GridType = Dune::UGGrid<dim>;
#else
#error "no grid manager found. provide ugggrid"
#endif
  };
}

#endif // DUNEURO_DEFAULTGRIDS_HH
