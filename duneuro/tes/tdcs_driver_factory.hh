#ifndef DUNEURO_TDCS_DRIVER_FACTORY_HH
#define DUNEURO_TDCS_DRIVER_FACTORY_HH

#include <duneuro/meeg/fitted_meeg_driver_data.hh>
#include <duneuro/tes/tdcs_driver_interface.hh>

namespace duneuro
{
  template <int dim>
  struct TDCSDriverData {
    FittedMEEGDriverData<dim> fittedData;
  };

  template <int dim>
  struct TDCSDriverFactory {
    static std::unique_ptr<TDCSDriverInterface<dim>>
    make_tdcs_driver(const Dune::ParameterTree& config,
                     const TDCSDriverData<dim>& data = TDCSDriverData<dim>(),
                     DataTree dataTree = DataTree());
  };
}

#include <duneuro/tes/tdcs_driver_factory_impl.hh>

#endif // DUNEURO_TDCS_DRIVER_FACTORY_HH
