
#include <config.h>

#include <duneuro/tes/fitted_tdcs_driver.hh>

template class duneuro::FittedTDCSDriver<2, duneuro::ElementType::tetrahedron,
                                         duneuro::FittedSolverType::dg, 1>;
template class duneuro::FittedTDCSDriver<2, duneuro::ElementType::hexahedron,
                                         duneuro::FittedSolverType::dg, 1>;
