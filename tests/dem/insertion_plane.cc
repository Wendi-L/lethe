// SPDX-FileCopyrightText: Copyright (c) 2023-2025 The Lethe Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception OR LGPL-2.1-or-later

/**
 * @brief Insertion of particles using the plane insertion class.
 */

// Deal.II includes
#include <deal.II/fe/mapping_q.h>

#include <deal.II/grid/grid_generator.h>

#include <deal.II/particles/particle.h>

// Lethe
#include <dem/dem_solver_parameters.h>
#include <dem/insertion_plane.h>

// Tests (with common definitions)
#include <../tests/tests.h>

using namespace dealii;

template <int dim, typename PropertiesIndex>
void
test()
{
  // Creating the mesh and refinement
  parallel::distributed::Triangulation<dim> tr(MPI_COMM_WORLD);
  int                                       hyper_cube_length = 2;
  GridGenerator::hyper_cube(tr,
                            -1 * hyper_cube_length,
                            hyper_cube_length,
                            true);
  int refinement_number = 2;
  tr.refine_global(refinement_number);

  MappingQ<dim>            mapping(1);
  DEMSolverParameters<dim> dem_parameters;

  // Defining simulation general parameters
  dem_parameters.insertion_info.insertion_plane_normal_vector =
    Tensor<1, 3>({0., 1., 0.});
  dem_parameters.insertion_info.insertion_plane_point = Point<3>({0., 1.75, 0});
  dem_parameters.insertion_info.distance_threshold    = 0.25;
  dem_parameters.lagrangian_physical_properties.particle_type_number = 1;
  dem_parameters.lagrangian_physical_properties.particle_average_diameter[0] =
    0.2;
  dem_parameters.lagrangian_physical_properties.distribution_type.push_back(
    Parameters::Lagrangian::SizeDistributionType::uniform);
  dem_parameters.lagrangian_physical_properties.particle_size_std[0] = 0;
  dem_parameters.lagrangian_physical_properties.density_particle[0]  = 2500;
  dem_parameters.lagrangian_physical_properties.number[0]            = 16;
  dem_parameters.insertion_info.insertion_maximum_offset             = 0.2;
  dem_parameters.insertion_info.seed_for_insertion                   = 19;
  dem_parameters.insertion_info.insertion_frequency                  = 2;

  // Calling uniform insertion
  std::vector<std::shared_ptr<Distribution>> distribution_object_container;
  distribution_object_container.push_back(std::make_shared<UniformDistribution>(
    dem_parameters.lagrangian_physical_properties
      .particle_average_diameter[0]));

  // Calling plane insertion
  InsertionPlane<dim, PropertiesIndex> insertion_object(
    distribution_object_container, tr, dem_parameters);

  // Defining particle handler
  Particles::ParticleHandler<dim> particle_handler(
    tr, mapping, PropertiesIndex::n_properties);

  insertion_object.insert(particle_handler, tr, dem_parameters);

  // Output
  int particle_number = 1;
  for (auto particle = particle_handler.begin();
       particle != particle_handler.end();
       ++particle, ++particle_number)
    {
      deallog << "Particle " << particle_number
              << " is inserted at: " << particle->get_location()[0] << " "
              << particle->get_location()[1] << " "
              << particle->get_location()[2] << " " << std::endl;
    }
}

int
main(int argc, char **argv)
{
  try
    {
      Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);

      initlog();
      test<3, DEM::DEMProperties::PropertiesIndex>();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl
                << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl
                << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  return 0;
}
