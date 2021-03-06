#ifndef DUNEURO_VOLUME_CONDUCTOR_DRIVER_FACTORY_HH
#define DUNEURO_VOLUME_CONDUCTOR_DRIVER_FACTORY_HH

#include <memory>

#include <dune/common/parametertree.hh>
#include <duneuro/common/fitted_driver_data.hh>
#include <duneuro/driver/volume_conductor_interface.hh>
#include <duneuro/io/data_tree.hh>

#if HAVE_DUNE_UDG
#include <duneuro/driver/unfitted_meeg_driver_data.hh>
#endif

namespace duneuro {

template <int dim> class VolumeConductorFactory {
public:
  /**
   * \brief create a new volume conductor
   *
   * The type of the volume conductor is given by the `type` parameter in the \p config
   * ParameterTree. Currently supported are `fitted` and `udg`.
   *
   * - `fitted`: The fitted volume conductor uses a mesh to describe the geometry. It
   * takes two main parameters to select the appropriate driver.
   *    - `solver_type`: defines the type of the fitted solver. Currently
   * supported are `cg` and `dg`.
   *    - `element_type` : defines the type of the mesh element. Currently
   * supported are `tetrahedron` and `hexahedron`. For hexahedral meshes,
   * geometry adaption can be activated by setting the `geometry_adapted` option
   * to `true`. Note that `dune-subgrid` has to be available when using geometry
   * adapted meshes.
   * - `udg` : The udg volume conductor uses a structured mesh and level set functions to
   * describe the geometry. The number of compartments is set using the
   * `compartments` parameter. Currently, 4 and 5 compartments are supported.
   *
   * The configuration is passed on to the selected volume conductor.
   */
  static std::shared_ptr<VolumeConductorInterface<dim>>
  make_volume_conductor(Dune::ParameterTree config,
                        const MEEGDriverData<dim> &data,
                        DataTree dataTree = DataTree());
};
} // namespace duneuro

#include <duneuro/driver/volume_conductor_factory_impl.hh>

#endif // VOLUME_CONDUCTOR_DRIVER_FACTORY_HH
