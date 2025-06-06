// SPDX-FileCopyrightText: Copyright (c) 2022-2025 The Lethe Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception OR LGPL-2.1-or-later

#include <core/parameters_lagrangian.h>

#include <dem/particle_wall_linear_force.h>
#include <fem-dem/ib_particles_dem.h>

#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_values.h>

template <int dim>
void
IBParticlesDEM<dim>::initialize(
  const std::shared_ptr<Parameters::IBParticles<dim>> &p_nsparam,
  const std::shared_ptr<Parameters::Lagrangian::FloatingWalls<dim>>
                                      fw_parameters,
  const MPI_Comm                     &mpi_communicator_input,
  const std::vector<IBParticle<dim>> &particles)
{
  parameters                = p_nsparam;
  floating_walls_parameters = fw_parameters;
  mpi_communicator          = mpi_communicator_input;
  dem_particles             = particles;
  boundary_cells.resize(dem_particles.size());

  std::vector<types::boundary_id> boundary_index(0);
}
template <int dim>
void
IBParticlesDEM<dim>::update_particles(
  const std::vector<IBParticle<dim>> &particles,
  double                              time)
{
  dem_particles = particles;
  cfd_time      = time;
}

template <int dim>
void
IBParticlesDEM<dim>::update_contact_candidates()
{
  particles_contact_candidates.resize(dem_particles.size());

  double radius_factor = parameters->contact_search_radius_factor;

  for (auto &particle_one : dem_particles)
    {
      const Point<dim> particle_one_location = particle_one.position;
      for (auto &particle_two : dem_particles)
        {
          if (particle_one.particle_id < particle_two.particle_id)
            {
              const Point<dim> particle_two_location = particle_two.position;
              double           distance =
                (particle_one_location - particle_two_location).norm();
              if (typeid(*particle_one.shape) == typeid(Sphere<dim>) &&
                  typeid(*particle_two.shape) == typeid(Sphere<dim>))
                {
                  distance =
                    (particle_one_location - particle_two_location).norm();
                }
              else if (typeid(*particle_one.shape) == typeid(Sphere<dim>) &&
                       typeid(*particle_two.shape) != typeid(Sphere<dim>))
                {
                  distance = particle_two.shape->value(particle_one_location);
                }
              else if (typeid(*particle_one.shape) != typeid(Sphere<dim>) &&
                       typeid(*particle_two.shape) == typeid(Sphere<dim>))
                {
                  distance = particle_one.shape->value(particle_two_location);
                }
              else
                {
                  distance = ((particle_one.shape->get_rotation_matrix() *
                                 point_nd_to_3d(
                                   particle_one.shape->bounding_box_center) +
                               point_nd_to_3d(particle_one.position)) -
                              (particle_two.shape->get_rotation_matrix() *
                                 point_nd_to_3d(
                                   particle_two.shape->bounding_box_center) +
                               point_nd_to_3d(particle_two.position)))
                               .norm();
                }

              if (distance <
                  (particle_one.shape->bounding_box_half_length.norm() +
                   particle_two.shape->bounding_box_half_length.norm()) *
                    radius_factor)
                {
                  (particles_contact_candidates[particle_one.particle_id])
                    .insert(particle_two.particle_id);
                }
            }
        }
    }
}



template <int dim>
void
IBParticlesDEM<dim>::calculate_pp_contact_force(
  const double               dt_dem,
  std::vector<Tensor<1, 3>> &contact_force,
  std::vector<Tensor<1, 3>> &contact_torque)
{
  using dealii::numbers::PI;

  for (auto &particle_one : dem_particles)
    {
      std::set<unsigned int>::iterator particle_contact_candidates_id;
      for (particle_contact_candidates_id =
             particles_contact_candidates[particle_one.particle_id].begin();
           particle_contact_candidates_id !=
           particles_contact_candidates[particle_one.particle_id].end();
           ++particle_contact_candidates_id)
        {
          const unsigned int particle_contact_id =
            *particle_contact_candidates_id;

          auto &particle_two = dem_particles[particle_contact_id];
          if (particle_one.particle_id < particle_two.particle_id)
            {
              ContactInfo contact_info;
              // Check if there is already information on the contact of
              // these to particles. If not initialize it in the contact map
              // with 0 values.
              try
                {
                  contact_info = pp_contact_map[particle_one.particle_id]
                                               [particle_two.particle_id];
                }
              catch (...)
                {
                  for (int d = 0; d < dim; ++d)
                    {
                      contact_info.tangential_displacement[d]          = 0.;
                      contact_info.rolling_resistance_spring_torque[d] = 0.;
                    }
                  pp_contact_map[particle_one.particle_id]
                                [particle_two.particle_id] = contact_info;
                }

              // Calculation of normal overlap
              double                  normal_overlap;
              Point<dim>              contact_point;
              Tensor<1, dim>          normal;
              std::vector<Point<dim>> contact_points_candidate;

              auto iterator =
                previous_wall_contact_point[particle_one.particle_id].find(
                  particle_two.particle_id);
              if (iterator !=
                  previous_wall_contact_point[particle_one.particle_id].end())
                {
                  contact_points_candidate.push_back(iterator->second);
                }
              else
                {
                  contact_points_candidate.push_back(
                    particle_one.position +
                    (particle_two.position - particle_one.position) *
                      (particle_one.radius /
                       (particle_one.radius + particle_two.radius)));
                }
              // The normal vector goes from particle one to particle two.
              if (typeid(*particle_one.shape) == typeid(Sphere<dim>) &&
                  typeid(*particle_two.shape) == typeid(Sphere<dim>))
                {
                  auto contact_state = particle_one.shape->distance_to_shape(
                    *particle_two.shape, contact_points_candidate);
                  normal_overlap = -std::get<double>(contact_state);
                  contact_point  = std::get<Point<dim>>(contact_state);
                  normal         = std::get<Tensor<1, dim>>(contact_state);
                }
              else if (typeid(*particle_one.shape) == typeid(Sphere<dim>) &&
                       typeid(*particle_two.shape) != typeid(Sphere<dim>))
                {
                  auto contact_state = particle_one.shape->distance_to_shape(
                    *particle_two.shape, contact_points_candidate);
                  normal_overlap = -std::get<double>(contact_state);
                  contact_point  = std::get<Point<dim>>(contact_state);
                  normal         = std::get<Tensor<1, dim>>(contact_state);
                }
              else if (typeid(*particle_one.shape) != typeid(Sphere<dim>) &&
                       typeid(*particle_two.shape) == typeid(Sphere<dim>))
                {
                  auto contact_state = particle_two.shape->distance_to_shape(
                    *particle_one.shape, contact_points_candidate);
                  normal_overlap = -std::get<double>(contact_state);
                  contact_point  = std::get<Point<dim>>(contact_state);
                  normal         = -std::get<Tensor<1, dim>>(
                    contact_state); // Flip the normal since it returns the
                                            // normal using the second particle;
                }
              else
                {
                  auto contact_state = particle_one.shape->distance_to_shape(
                    *particle_two.shape, contact_points_candidate);
                  normal_overlap = -std::get<double>(contact_state);
                  contact_point  = std::get<Point<dim>>(contact_state);
                  normal         = std::get<Tensor<1, dim>>(contact_state);
                }
              previous_wall_contact_point[particle_one.particle_id]
                                         [particle_two.particle_id] =
                                           contact_point;
              Point<3>     contact_point_3d = point_nd_to_3d(contact_point);
              Tensor<1, 3> contact_normal   = tensor_nd_to_3d(normal);


              if (normal_overlap > 0)
                // This means that the adjacent particles are in contact
                {
                  double contact_radius_particle_one;
                  double contact_radius_particle_two;
                  if (parameters->approximate_radius_for_contact)
                    {
                      contact_radius_particle_one = particle_one.radius;
                      contact_radius_particle_two = particle_two.radius;
                    }
                  else
                    {
                      contact_radius_particle_one =
                        particle_one.shape->local_curvature_radius(
                          contact_point);
                      contact_radius_particle_two =
                        particle_two.shape->local_curvature_radius(
                          contact_point);
                    }
                  Tensor<1, 3> normal_force;
                  Tensor<1, 3> tangential_force;
                  Tensor<1, 3> particle_one_tangential_torque;
                  Tensor<1, 3> particle_two_tangential_torque;
                  Tensor<1, 3> rolling_resistance_torque;
                  Tensor<1, 3> particle_one_velocity_3d =
                    tensor_nd_to_3d(particle_one.velocity);
                  Tensor<1, 3> particle_two_velocity_3d =
                    tensor_nd_to_3d(particle_two.velocity);

                  ObjectProperties particle_one_properties;
                  particle_one_properties.object_friction_coefficient =
                    particle_one.friction_coefficient;
                  particle_one_properties.object_mass = particle_one.mass;
                  particle_one_properties.object_poisson_ratio =
                    particle_one.poisson_ratio;
                  particle_one_properties.object_radius =
                    contact_radius_particle_one;
                  particle_one_properties.object_restitution_coefficient =
                    particle_one.restitution_coefficient;
                  particle_one_properties.object_rolling_friction_coefficient =
                    particle_one.rolling_friction_coefficient;
                  particle_one_properties.object_youngs_modulus =
                    particle_one.youngs_modulus;
                  ObjectProperties particle_two_properties;
                  particle_two_properties.object_friction_coefficient =
                    particle_two.friction_coefficient;
                  particle_two_properties.object_mass = particle_two.mass;
                  particle_two_properties.object_poisson_ratio =
                    particle_two.poisson_ratio;
                  particle_two_properties.object_radius =
                    contact_radius_particle_two;
                  particle_two_properties.object_restitution_coefficient =
                    particle_two.restitution_coefficient;
                  particle_two_properties.object_rolling_friction_coefficient =
                    particle_two.rolling_friction_coefficient;
                  particle_two_properties.object_youngs_modulus =
                    particle_two.youngs_modulus;

                  calculate_force_model(normal_overlap,
                                        contact_info,
                                        contact_point_3d,
                                        contact_normal,
                                        normal_force,
                                        tangential_force,
                                        rolling_resistance_torque,
                                        particle_one.position,
                                        particle_one_velocity_3d,
                                        particle_one.omega,
                                        particle_one_properties,
                                        particle_two.position,
                                        particle_two_velocity_3d,
                                        particle_two.omega,
                                        particle_two_properties,
                                        dt_dem);


                  contact_force[particle_one.particle_id] -=
                    (normal_force + tangential_force);
                  contact_force[particle_two.particle_id] +=
                    (normal_force + tangential_force);

                  contact_torque[particle_one.particle_id] =
                    contact_torque[particle_one.particle_id] -
                    particle_one_tangential_torque + rolling_resistance_torque +
                    cross_product_3d((contact_point_3d -
                                      point_nd_to_3d(particle_one.position)),
                                     -(normal_force + tangential_force));
                  contact_torque[particle_two.particle_id] =
                    contact_torque[particle_two.particle_id] -
                    particle_two_tangential_torque - rolling_resistance_torque +
                    cross_product_3d((contact_point_3d -
                                      point_nd_to_3d(particle_two.position)),
                                     (normal_force + tangential_force));
                  ;
                }

              else
                {
                  // if the adjacent pair is not in contact anymore
                  for (int d = 0; d < dim; ++d)
                    {
                      contact_info.tangential_displacement[d]          = 0.;
                      contact_info.rolling_resistance_spring_torque[d] = 0.;
                    }
                  pp_contact_map[particle_one.particle_id].erase(
                    particle_two.particle_id);
                }
            }
        }
    }
}

template <int dim>
void
IBParticlesDEM<dim>::calculate_pp_lubrication_force(
  const double /*dt_dem*/,
  const double               h_max,
  const double               h_min,
  const double               mu,
  std::vector<Tensor<1, 3>> &lubrication_force,
  std::vector<Tensor<1, 3>> & /*lubrication_torque*/)
{
  using dealii::numbers::PI;

  // loop over all particles to find pair of close particles
  for (auto &particle_one : dem_particles)
    {
      for (auto particle_contact_candidates_id =
             particles_contact_candidates[particle_one.particle_id].begin();
           particle_contact_candidates_id !=
           particles_contact_candidates[particle_one.particle_id].end();
           ++particle_contact_candidates_id)
        {
          const auto &particle_contact_id = *particle_contact_candidates_id;
          auto       &particle_two        = dem_particles[particle_contact_id];
          if (particle_one.particle_id < particle_two.particle_id)
            {
              const Point<dim> particle_one_location = particle_one.position;
              const Point<dim> particle_two_location = particle_two.position;
              Tensor<1, 3>     radial_vector;
              double           radial_velocity;
              Tensor<1, 3>     f_lub;

              // Calculation of normal overlap
              double gap =
                particle_one_location.distance(particle_two_location) -
                (particle_one.radius + particle_two.radius);
              radial_vector =
                tensor_nd_to_3d(particle_one.position - particle_two.position);
              if (gap > 0 and gap < h_max)
                // This means that the adjacent particles are very close but
                // not in contact
                {
                  // Limit the smallest gap calculated
                  if (gap < h_min)
                    {
                      gap = h_min;
                    }

                  // Evaluate the force
                  radial_velocity =
                    scalar_product(-radial_vector, particle_one.velocity) +
                    scalar_product(radial_vector, particle_two.velocity);
                  f_lub =
                    3 / 2 * PI * mu *
                      (particle_one.radius * 2 * particle_two.radius * 2 /
                       (particle_one.radius * 2 + particle_two.radius * 2)) *
                      (particle_one.radius * 2 * particle_two.radius * 2 /
                       (particle_one.radius * 2 + particle_two.radius * 2)) /
                      gap * radial_velocity * radial_vector /
                      radial_vector.norm() -
                    3 / 2 * PI * mu *
                      (particle_one.radius * 2 * particle_two.radius * 2 /
                       (particle_one.radius * 2 + particle_two.radius * 2)) *
                      (particle_one.radius * 2 * particle_two.radius * 2 /
                       (particle_one.radius * 2 + particle_two.radius * 2)) /
                      h_max * radial_velocity * radial_vector /
                      radial_vector.norm();
                  ;
                }

              lubrication_force[particle_one.particle_id] = f_lub;
              lubrication_force[particle_two.particle_id] = -f_lub;
            }
        }
    }
}

template <int dim>
void
IBParticlesDEM<dim>::update_particles_boundary_contact(
  std::vector<IBParticle<dim>> &particles,
  const DoFHandler<dim>        &dof_handler,
  const Quadrature<dim - 1>    &face_quadrature_formula,
  const Mapping<dim>           &mapping)
{
  const FESystem<dim, dim>                    fe = dof_handler.get_fe();
  std::vector<std::map<unsigned int, double>> best_contact_candidate(
    particles.size());
  for (unsigned int p_i = 0; p_i < particles.size(); ++p_i)
    {
      // Clear the last boundary cell candidates.
      boundary_cells[p_i].clear();

      // Find the new cells that are at a boundary and in proximity of the
      // particle.
      auto cells_at_boundary = LetheGridTools::find_boundary_cells_in_sphere(
        dof_handler,
        particles[p_i].position,
        particles[p_i].radius * parameters->contact_search_radius_factor);


      // Loop over the cells at the boundary.
      for (unsigned int i = 0; i < cells_at_boundary.size(); ++i)
        {
          unsigned int      n_face_q_points = face_quadrature_formula.size();
          FEFaceValues<dim> fe_face_values(mapping,
                                           fe,
                                           face_quadrature_formula,
                                           update_values |
                                             update_quadrature_points |
                                             update_normal_vectors);
          // Loop over the faces of the cell at the boundary.
          for (int face_id = 0;
               face_id < int(GeometryInfo<dim>::faces_per_cell);
               ++face_id)
            {
              // Find the face at the boundary
              if (cells_at_boundary[i]->face(face_id)->at_boundary())
                {
                  fe_face_values.reinit(cells_at_boundary[i], face_id);
                  // Loop over the quadrature point of the face at the boundary
                  // to store information about the location and normals of the
                  // boundary.
                  for (unsigned int f_q_point = 0; f_q_point < n_face_q_points;
                       ++f_q_point)
                    {
                      Tensor<1, dim> normal_vector =
                        -fe_face_values.normal_vector(f_q_point);
                      BoundaryCellsInfo boundary_information;
                      boundary_information.normal_vector = normal_vector;
                      boundary_information.point_on_boundary =
                        fe_face_values.quadrature_point(f_q_point);
                      boundary_information.boundary_index =
                        cells_at_boundary[i]->face(face_id)->boundary_id();
                      auto iterator = best_contact_candidate[p_i].find(
                        boundary_information.boundary_index);

                      double level_set = particles[p_i].get_levelset(
                        boundary_information.point_on_boundary);
                      if (iterator != best_contact_candidate[p_i].end())
                        {
                          if (level_set <
                              best_contact_candidate[p_i][boundary_information
                                                            .boundary_index])
                            {
                              boundary_cells[p_i][boundary_information
                                                    .boundary_index] =
                                boundary_information;
                              best_contact_candidate[p_i][boundary_information
                                                            .boundary_index] =
                                level_set;
                            }
                        }
                      else
                        {
                          boundary_cells[p_i]
                                        [boundary_information.boundary_index] =
                                          boundary_information;
                          best_contact_candidate[p_i][boundary_information
                                                        .boundary_index] =
                            level_set;
                        }
                    }
                }
            }
        }

      // Regroup the information of all the processor
      auto global_boundary_cell =
        Utilities::MPI::all_gather(this->mpi_communicator, boundary_cells[p_i]);
      boundary_cells[p_i].clear();
      for (unsigned int i = 0; i < global_boundary_cell.size(); ++i)
        {
          boundary_cells[p_i].insert(global_boundary_cell[i].begin(),
                                     global_boundary_cell[i].end());
        }
    }
}


template <int dim>
void
IBParticlesDEM<dim>::calculate_pw_contact_force(
  const double               dt_dem,
  std::vector<Tensor<1, 3>> &contact_force,
  std::vector<Tensor<1, 3>> &contact_torque)
{
  ObjectProperties wall_properties;
  wall_properties.object_friction_coefficient =
    parameters->wall_friction_coefficient;
  wall_properties.object_poisson_ratio = parameters->wall_poisson_ratio;
  wall_properties.object_restitution_coefficient =
    parameters->wall_restitution_coefficient;
  wall_properties.object_rolling_friction_coefficient =
    parameters->wall_rolling_friction_coefficient;
  wall_properties.object_youngs_modulus = parameters->wall_youngs_modulus;

  using dealii::numbers::PI;

  // Loop over the particles
  for (auto &particle : dem_particles)
    {
      if (particle.integrate_motion)
        {
          // Defines map with default values.
          unsigned int                              boundary_index = 0;
          std::map<unsigned int, DefaultDBL_MAX>    best_dist;
          std::map<unsigned int, DefaultUINT_MAX>   best_indices;
          std::map<unsigned int, BoundaryCellsInfo> best_cells;
          // Loop over the point and normal identified as
          // potential contact candidate.
          for (auto boundary_cell_iter =
                 boundary_cells[particle.particle_id].begin();
               boundary_cell_iter != boundary_cells[particle.particle_id].end();
               boundary_cell_iter++)
            {
              // Find the best candidate (the closest point) for each
              // different wall.
              double dist = particle.get_levelset(
                boundary_cell_iter->second.point_on_boundary);

              // Check if the distance is smaller than the best distance.
              // If it is the first time this boundary is encountered, the
              // distance is compared to the default value of the map, which
              // is DBL_MAX
              if (dist <
                  best_dist[boundary_cell_iter->second.boundary_index].value)
                {
                  best_dist[boundary_cell_iter->second.boundary_index].value =
                    dist;
                  best_indices[boundary_cell_iter->second.boundary_index]
                    .value = boundary_index;
                  best_cells[boundary_cell_iter->second.boundary_index] =
                    boundary_cell_iter->second;
                }
              boundary_index += 1;
            }

          // Add all the floating walls as contact candidates. Their indices
          // start from 1M (defined in the definition of:
          // lowest_floating_wall_indices). This prevents a floating wall
          // from sharing the same indices as a normal boundary of the
          // domain in the contact candidates list.
          for (unsigned int i = 0;
               i < floating_walls_parameters->points_on_walls.size();
               ++i)
            {
              if (floating_walls_parameters->time_start[i] < cfd_time &&
                  floating_walls_parameters->time_end[i] > cfd_time)
                {
                  BoundaryCellsInfo floating_wall_cell_info;
                  floating_wall_cell_info.point_on_boundary =
                    floating_walls_parameters->points_on_walls[i];
                  floating_wall_cell_info.normal_vector =
                    floating_walls_parameters->floating_walls_normal_vectors[i];
                  best_cells[lowest_floating_wall_indices + i + 1] =
                    floating_wall_cell_info;
                }
            }

          // Do the particle wall contact calculation with the best
          // candidate.
          for (auto best_index_of_face = best_cells.begin();
               best_index_of_face != best_cells.end();
               ++best_index_of_face)
            {
              auto &boundary_cell = best_index_of_face->second;

              auto        boundary_cell_information = boundary_cell;
              ContactInfo contact_info;

              // Check if there is already information on the contact
              // between this particle and this boundary contact point. If
              // not initialize the contact history with 0 values.
              try
                {
                  contact_info = pw_contact_map[particle.particle_id]
                                               [boundary_cell.boundary_index];
                }
              catch (...)
                {
                  for (int d = 0; d < dim; ++d)
                    {
                      contact_info.tangential_displacement[d]      = 0;
                      contact_info.tangential_relative_velocity[d] = 0;
                    }

                  contact_info.normal_overlap           = 0;
                  contact_info.normal_relative_velocity = 0;

                  pw_contact_map[particle.particle_id]
                                [boundary_cell.boundary_index] = contact_info;
                }

              Tensor<1, 3> normal =
                tensor_nd_to_3d(boundary_cell_information.normal_vector);
              auto point_on_boundary =
                boundary_cell_information.point_on_boundary;

              Point<3> particle_position_3d = point_nd_to_3d(particle.position);
              Tensor<1, 3> particle_velocity_3d =
                tensor_nd_to_3d(particle.velocity);

              // Finding the projected vector on the normal vector of the
              // boundary. Here we have used the private function
              // find_projection. Using this projected vector, the
              // particle-wall distance is calculated

              // Create a Rotation matrix from normal to z axis to
              // initialize the plane for contact with the right
              // orientation;
              Tensor<1, 3> rotation_axis;
              double       angle = 0;
              if constexpr (dim == 2)
                {
                  rotation_axis[2] = 1;
                  angle            = std::atan2(-normal[0], normal[1]);
                }
              else
                {
                  angle =
                    std::acos(scalar_product(Tensor<1, 3>({0, 0, 1}), normal) /
                              normal.norm());
                  if (abs(scalar_product(Tensor<1, 3>({0, 0, 1}), normal)) !=
                      normal.norm())
                    {
                      rotation_axis =
                        cross_product_3d(Tensor<1, 3>({0, 0, 1}), normal);
                      rotation_axis = rotation_axis / rotation_axis.norm();
                    }
                  else
                    {
                      rotation_axis = Tensor<1, 3>({0, 1, 0});
                    }
                }
              Tensor<2, 3> rotation_matrix =
                Physics::Transformations::Rotations::rotation_matrix_3d(
                  rotation_axis, angle);
              Tensor<1, 3> orientation =
                particle.shape->rotation_matrix_to_xyz_angles(rotation_matrix);

              std::shared_ptr<Shape<dim>> contact_plane =
                std::make_shared<Plane<dim>>(point_on_boundary, orientation);
              contact_plane->set_position(point_on_boundary);
              contact_plane->set_orientation(orientation);


              std::vector<Point<dim>> contact_point_candidate;
              auto                    iterator =
                previous_wall_contact_point[particle.particle_id].find(
                  boundary_cell.boundary_index);
              if (iterator !=
                  previous_wall_contact_point[particle.particle_id].end())
                {
                  contact_point_candidate.push_back(iterator->second);
                }
              else
                {
                  contact_point_candidate.push_back(point_on_boundary);
                }
              // Use the last contact point as an initial guess if the level
              // set is smaller than the wall initial guess. Find the normal
              // overlap
              auto contact_state =
                particle.shape->distance_to_shape(*contact_plane,
                                                  contact_point_candidate);
              double   normal_overlap = -2 * std::get<double>(contact_state);
              Point<3> contact_point =
                point_nd_to_3d(std::get<Point<dim>>(contact_state));
              // The normal vector goes from particle one to particle two.
              // Here, particle two is the wall. As such, we flip the
              // normal. Here, particle two is the wall. As such, we flip
              // the normal.
              Tensor<1, 3> contact_normal = -normal;


              // Keep the last contact point as an initial guess for the
              // next contact point.
              previous_wall_contact_point[particle.particle_id]
                                         [boundary_cell.boundary_index] =
                                           std::get<Point<dim>>(contact_state);

              if (normal_overlap > 0)
                {
                  // Evaluates the curvature radius at the
                  // contact point.
                  double contact_radius_particle_one;

                  if (parameters->approximate_radius_for_contact)
                    {
                      contact_radius_particle_one = particle.radius;
                    }
                  else
                    {
                      contact_radius_particle_one =
                        particle.shape->local_curvature_radius(
                          std::get<Point<dim>>(contact_state));
                    }
                  // Do the calculation to evaluate the particle wall
                  // contact force.
                  contact_info.normal_overlap = normal_overlap;

                  Tensor<1, 3> normal_force;
                  Tensor<1, 3> tangential_force;
                  Tensor<1, 3> tangential_torque;
                  Tensor<1, 3> rolling_resistance_torque;
                  Tensor<1, 3> wall_velocity;
                  Tensor<1, 3> wall_angular_velocity;
                  Point<dim>   wall_center_of_rotation;

                  pw_contact_map[particle.particle_id][boundary_index]
                    .normal_overlap = normal_overlap;
                  pw_contact_map[particle.particle_id][boundary_index]
                    .normal_vector = normal;

                  ObjectProperties particle_properties;
                  particle_properties.object_friction_coefficient =
                    particle.friction_coefficient;
                  particle_properties.object_mass = particle.mass;
                  particle_properties.object_poisson_ratio =
                    particle.poisson_ratio;
                  particle_properties.object_radius =
                    contact_radius_particle_one;
                  particle_properties.object_restitution_coefficient =
                    particle.restitution_coefficient;
                  particle_properties.object_rolling_friction_coefficient =
                    particle.rolling_friction_coefficient;
                  particle_properties.object_youngs_modulus =
                    particle.youngs_modulus;

                  // Wall mass is 100 times higher than
                  // the particles that contact it.
                  wall_properties.object_mass = particle.mass * 100;
                  // Wall curvature radius is 100 times higher than
                  // the particle that contacts it.
                  wall_properties.object_radius = particle.radius * 100;


                  calculate_force_model(normal_overlap,
                                        contact_info,
                                        contact_point,
                                        contact_normal,
                                        normal_force,
                                        tangential_force,
                                        rolling_resistance_torque,
                                        particle.position,
                                        particle_velocity_3d,
                                        particle.omega,
                                        particle_properties,
                                        wall_center_of_rotation,
                                        wall_velocity,
                                        wall_angular_velocity,
                                        wall_properties,
                                        dt_dem);

                  // Updating the force of particles in the particle handler
                  contact_force[particle.particle_id] -=
                    (normal_force + tangential_force);
                  // Updating the torque acting on particles
                  contact_torque[particle.particle_id] +=
                    tangential_torque + rolling_resistance_torque +
                    cross_product_3d((contact_point - particle_position_3d),
                                     -(normal_force + tangential_force));
                }
              else
                {
                  // Set to 0 the tangential displacement if the particle is not
                  // in contact with the wall anymore.
                  for (int d = 0; d < dim; ++d)
                    {
                      contact_info.tangential_displacement[d]          = 0.;
                      contact_info.rolling_resistance_spring_torque[d] = 0.;
                    }
                }
            }
        }
    }
}


template <int dim>
void
IBParticlesDEM<dim>::calculate_pw_lubrication_force(
  const double /*dt_dem*/,
  const double               h_max,
  const double               h_min,
  const double               mu,
  std::vector<Tensor<1, 3>> &lubrication_force,
  std::vector<Tensor<1, 3>> & /*lubrication_torque*/)
{
  using dealii::numbers::PI;

  // Loop over the particles
  for (auto &particle : dem_particles)
    {
      if (particle.integrate_motion)
        {
          // Defines map with default values.
          unsigned int                              boundary_index = 0;
          std::map<unsigned int, DefaultDBL_MAX>    best_dist;
          std::map<unsigned int, DefaultUINT_MAX>   best_indices;
          std::map<unsigned int, BoundaryCellsInfo> best_cells;
          // For each particle loop over the point and normal identified as
          // potential contact candidate.
          for (auto boundary_cell_iter =
                 boundary_cells[particle.particle_id].begin();
               boundary_cell_iter != boundary_cells[particle.particle_id].end();
               boundary_cell_iter++)
            {
              // Find the best candidate (the closest point) for each
              // different wall.

              double dist = particle.get_levelset(
                boundary_cell_iter->second.point_on_boundary);

              // Check if the distance is smaller than the best distance.
              // If it is the first time this boundary is encountered, the
              // distance is compared to the default value of the map, which
              // is DBL_MAX
              if (dist <
                  best_dist[boundary_cell_iter->second.boundary_index].value)
                {
                  best_dist[boundary_cell_iter->second.boundary_index].value =
                    dist;
                  best_indices[boundary_cell_iter->second.boundary_index]
                    .value = boundary_index;
                  best_cells[boundary_cell_iter->second.boundary_index] =
                    boundary_cell_iter->second;
                }
              boundary_index += 1;
            }


          // Add all the floating wall has contact candidate. Their indices
          // start from 1M (define in the definition of:
          // lowest_floating_wall_indices). This prevents a floating wall
          // from sharing the same indices as a normal boundary of the
          // domain in the contact candidates list.
          for (unsigned int i = 0;
               i < floating_walls_parameters->points_on_walls.size();
               ++i)
            {
              if (floating_walls_parameters->time_start[i] < cfd_time &&
                  floating_walls_parameters->time_end[i] > cfd_time)
                {
                  BoundaryCellsInfo floating_wall_cell_info;
                  floating_wall_cell_info.point_on_boundary =
                    floating_walls_parameters->points_on_walls[i];
                  floating_wall_cell_info.normal_vector =
                    floating_walls_parameters->floating_walls_normal_vectors[i];
                  best_cells[lowest_floating_wall_indices + i + 1] =
                    floating_wall_cell_info;
                }
            }

          // Do the particle wall contact calculation with the best
          // candidate.
          for (auto best_index_of_face = best_cells.begin();
               best_index_of_face != best_cells.end();
               ++best_index_of_face)
            {
              auto &boundary_cell = best_index_of_face->second;

              auto         boundary_cell_information = boundary_cell;
              Tensor<1, 3> normal =
                tensor_nd_to_3d(boundary_cell_information.normal_vector);
              auto point_on_boundary =
                boundary_cell_information.point_on_boundary;

              // Calculation of gap
              Point<3> particle_position_3d = point_nd_to_3d(particle.position);
              Point<3> point_on_boundary_3d = point_nd_to_3d(point_on_boundary);

              Tensor<1, 3> point_to_particle_vector =
                particle_position_3d - point_on_boundary_3d;

              // Finding the projected vector on the normal vector of the
              // boundary. Here we have used the private function
              // find_projection. Using this projected vector, the
              // particle-wall distance is calculated.
              Tensor<1, 3> projected_vector =
                ((point_to_particle_vector * normal) / (normal.norm_square())) *
                normal;

              double gap = (projected_vector.norm()) - particle.radius;

              Tensor<1, 3> radial_vector;
              double       radial_velocity;
              Tensor<1, 3> f_lub;

              radial_vector = projected_vector;
              if (gap > 0 and gap < h_max)
                // This means that the particles is very close to the wall
                // but not in contact
                {
                  // Limit the smallest gap calculated
                  if (gap < h_min)
                    {
                      gap = h_min;
                    }
                  // Evaluate the force
                  radial_velocity =
                    scalar_product(-radial_vector, particle.velocity);
                  f_lub = 3 / 2 * PI * mu * (particle.radius) *
                            (particle.radius) / gap * radial_velocity *
                            radial_vector / radial_vector.norm() -
                          3 / 2 * PI * mu * (particle.radius) *
                            (particle.radius) / h_max * radial_velocity *
                            radial_vector / radial_vector.norm();
                }
              lubrication_force[particle.particle_id] = f_lub;
            }
        }
    }
}


template <int dim>
void
IBParticlesDEM<dim>::calculate_force_model(
  const double           normal_overlap,
  ContactInfo           &contact_info,
  Point<3>              &contact_point,
  Tensor<1, 3>          &contact_normal,
  Tensor<1, 3>          &normal_force,
  Tensor<1, 3>          &tangential_force,
  Tensor<1, 3>          &rolling_resistance_torque,
  Point<dim>            &particle_one_position,
  Tensor<1, 3>          &particle_one_velocity,
  Tensor<1, 3>          &particle_one_omega,
  const ObjectProperties particle_one_properties,
  Point<dim>            &particle_two_position,
  Tensor<1, 3>          &particle_two_velocity,
  Tensor<1, 3>          &particle_two_omega,
  const ObjectProperties particle_two_properties,
  const double           dt)
{
  double effective_youngs_modulus;
  double effective_shear_modulus;
  double effective_coefficient_of_restitution;
  double effective_coefficient_of_friction;
  double effective_coefficient_of_rolling_friction;
  double model_parameter_beta;


  // Define local variables which will be used within the contact calculation
  //  Namely: normal and tangential contact forces, tangential and rolling
  //  torques, normal unit vector of the contact and contact relative velocity
  //  in the normal direction
  Tensor<1, 3> normal_unit_vector;
  double       normal_relative_velocity_value;
  Tensor<1, 3> tangential_relative_velocity;

  // DEM::PropertiesIndex::type is the first (0) property of particles in the
  // DEM solver. For the IB particles, the first property is ID. For force and
  // torque calculations, we need pairwise properties (such as effective
  // Young's modulus, effective coefficient of restitution, etc.) We rewrite
  // these pairwise properties by using the ID of IB particles (using
  // DEM::PropertiesIndex::type) and use them in force calculations.

  effective_youngs_modulus =
    (particle_one_properties.object_youngs_modulus *
     particle_two_properties.object_youngs_modulus) /
    ((particle_two_properties.object_youngs_modulus *
      (1.0 - particle_one_properties.object_poisson_ratio *
               particle_one_properties.object_poisson_ratio)) +
     (particle_one_properties.object_youngs_modulus *
      (1.0 - particle_two_properties.object_poisson_ratio *
               particle_two_properties.object_poisson_ratio)) +
     DBL_MIN);

  effective_shear_modulus =
    (particle_one_properties.object_youngs_modulus *
     particle_one_properties.object_youngs_modulus) /
    (2.0 * ((particle_two_properties.object_youngs_modulus *
             (2.0 - particle_one_properties.object_poisson_ratio) *
             (1.0 + particle_two_properties.object_poisson_ratio)) +
            (particle_one_properties.object_youngs_modulus *
             (2.0 - particle_two_properties.object_poisson_ratio) *
             (1.0 + particle_two_properties.object_poisson_ratio))) +
     DBL_MIN);

  effective_coefficient_of_restitution =
    harmonic_mean(particle_one_properties.object_restitution_coefficient,
                  particle_two_properties.object_restitution_coefficient);
  effective_coefficient_of_friction =
    harmonic_mean(particle_one_properties.object_friction_coefficient,
                  particle_two_properties.object_friction_coefficient);
  effective_coefficient_of_rolling_friction =
    harmonic_mean(particle_one_properties.object_rolling_friction_coefficient,
                  particle_two_properties.object_rolling_friction_coefficient);

  const double restitution_coefficient_particle_log =
    std::log(effective_coefficient_of_restitution);

  model_parameter_beta = restitution_coefficient_particle_log /
                         sqrt(restitution_coefficient_particle_log *
                                restitution_coefficient_particle_log +
                              9.8696);

  // Since the normal overlap is already calculated we update
  // this element of the container here. The rest of information
  // are updated using the following function

  ////////////// Contact info ////////////////

  // Calculation of the normal unit contact vector
  normal_unit_vector = contact_normal;

  // Defining relative contact velocity
  Tensor<1, 3> contact_relative_velocity;

  // Assigning velocities and angular velocities of particles
  Tensor<1, 3> velocity_of_particle_one_at_contact_point =
    particle_one_velocity +
    cross_product_3d(particle_one_omega,
                     (contact_point - point_nd_to_3d(particle_one_position)));

  Tensor<1, 3> velocity_of_particle_two_at_contact_point =
    particle_two_velocity +
    cross_product_3d(particle_two_omega,
                     (contact_point - point_nd_to_3d(particle_two_position)));
  contact_relative_velocity = (velocity_of_particle_one_at_contact_point -
                               velocity_of_particle_two_at_contact_point);

  // Calculation of normal relative velocity. Note that in the
  // following line the product acts as inner product since both
  // sides are vectors, while in the second line the product is
  // scalar and vector product
  normal_relative_velocity_value =
    contact_relative_velocity * normal_unit_vector;

  // Calculation of tangential relative velocity
  // v_rt = v_ij - (v_ij⋅n_ij)*n_ij
  tangential_relative_velocity =
    contact_relative_velocity -
    (normal_relative_velocity_value * normal_unit_vector);

  // Calculation of new tangential_displacement, since this value is
  // history-dependent it needs the value at previous time-step
  // This variable is the main reason that we have iterations over
  // two different vectors : tangential_displacement of the particles
  // which were already in contact needs to be
  // modified using its history, while the tangential_displacements of
  // new particles are equal to zero
  // delta_t_new = delta_t_old + v_rt*dt
  contact_info.tangential_relative_velocity.push_back(
    tangential_relative_velocity);
  contact_info.tangential_displacement += tangential_relative_velocity * dt;

  ///////////// Hertz contact force model ////////////////
  // Calculation of effective radius and mass
  double effective_mass =
    (particle_one_properties.object_mass *
     particle_two_properties.object_mass) /
    (particle_one_properties.object_mass + particle_two_properties.object_mass);
  double effective_radius = (particle_one_properties.object_radius *
                             particle_two_properties.object_radius) /
                            ((particle_one_properties.object_radius +
                              particle_two_properties.object_radius));

  const double radius_times_overlap_sqrt =
    sqrt(effective_radius * normal_overlap);
  const double model_parameter_sn =
    2 * effective_youngs_modulus * radius_times_overlap_sqrt;

  // Calculation of normal and tangential spring and dashpot constants
  // using particle properties
  double normal_spring_constant = 0.66665 * model_parameter_sn;
  double normal_damping_constant =
    -1.8257 * model_parameter_beta * sqrt(model_parameter_sn * effective_mass);
  double tangential_spring_constant =
    8.0 * effective_shear_modulus * radius_times_overlap_sqrt + DBL_MIN;

  // Calculation of normal force using spring and dashpot normal forces
  normal_force =
    ((normal_spring_constant * normal_overlap) * normal_unit_vector) +
    ((normal_damping_constant * normal_relative_velocity_value) *
     normal_unit_vector);

  // Calculation of tangential force using spring and dashpot tangential
  // forces. Since we need dashpot tangential force in the gross sliding
  // again, we define it as a separate variable
  tangential_force =
    tangential_spring_constant * contact_info.tangential_displacement;

  double coulomb_threshold =
    effective_coefficient_of_friction * normal_force.norm();

  // Check for gross sliding
  if (tangential_force.norm() > coulomb_threshold)
    {
      // Gross sliding occurs and the tangential displacement and tangential
      // force are limited to Coulomb's criterion
      tangential_force =
        coulomb_threshold *
        (tangential_force / (tangential_force.norm() + DBL_MIN));
    }

  // For calculation of rolling resistance torque, we need to obtain
  // omega_ij using rotational velocities of particles one and two
  Tensor<1, 3> omega_ij           = particle_one_omega - particle_two_omega;
  Tensor<1, 3> omega_ij_direction = omega_ij / (omega_ij.norm() + DBL_MIN);

  // Calculation of the linear rolling resistance torque
  rolling_resistance_torque =
    (-effective_coefficient_of_rolling_friction * effective_radius *
     normal_force.norm() * omega_ij_direction);
}



template <int dim>
void
IBParticlesDEM<dim>::integrate_particles_motion(const double dt,
                                                const double h_max,
                                                const double h_min,
                                                const double rho,
                                                const double mu)
{
  // Initialize local containers and physical variables
  using dealii::numbers::PI;
  double dt_dem = dt / parameters->coupling_frequency;

  std::vector<Tensor<1, 3>> contact_force(dem_particles.size());
  std::vector<Tensor<1, 3>> contact_wall_force(dem_particles.size());
  std::vector<Tensor<1, 3>> contact_torque(dem_particles.size());
  std::vector<Tensor<1, 3>> contact_wall_torque(dem_particles.size());
  std::vector<Tensor<1, 3>> current_fluid_force(dem_particles.size());
  std::vector<Tensor<1, 3>> current_fluid_torque(dem_particles.size());
  std::vector<Tensor<1, 3>> lubrication_force(dem_particles.size());
  std::vector<Tensor<1, 3>> lubrication_torque(dem_particles.size());
  std::vector<Tensor<1, 3>> lubrication_wall_force(dem_particles.size());
  std::vector<Tensor<1, 3>> lubrication_wall_torque(dem_particles.size());

  std::vector<Tensor<1, 3>> velocity(dem_particles.size());
  std::vector<Point<dim>>   position(dem_particles.size());

  // Local time for the dem step.
  double t = 0;
  // The gravitational acceleration.
  Tensor<1, 3> g;
  this->parameters->f_gravity->set_time(cfd_time + dt);
  // The gravitational force on the particle.
  Tensor<1, 3> gravity;
  // Reset the previous particle contact point list
  previous_wall_contact_point.clear();
  previous_particle_particle_contact_point.clear();

  // Initialize the particles
  for (unsigned int p_i = 0; p_i < dem_particles.size(); ++p_i)
    {
      dem_particles[p_i].position = dem_particles[p_i].previous_positions[0];
      dem_particles[p_i].velocity = dem_particles[p_i].previous_velocity[0];
      dem_particles[p_i].omega    = dem_particles[p_i].previous_omega[0];
      dem_particles[p_i].orientation =
        dem_particles[p_i].previous_orientation[0];
      dem_particles[p_i].impulsion               = 0;
      dem_particles[p_i].omega_impulsion         = 0;
      dem_particles[p_i].contact_impulsion       = 0;
      dem_particles[p_i].omega_contact_impulsion = 0;
      // Initialized the gravity at the particle position.
      g[0] = this->parameters->f_gravity->value(dem_particles[p_i].position, 0);
      g[1] = this->parameters->f_gravity->value(dem_particles[p_i].position, 1);
      if (dim == 3)
        g[2] =
          this->parameters->f_gravity->value(dem_particles[p_i].position, 2);
      dem_particles[p_i].set_position(dem_particles[p_i].position);
      dem_particles[p_i].set_orientation(dem_particles[p_i].orientation);
    }

  // Integrate with the sub_time_step
  while (t + 0.5 * dt_dem < dt)
    {
      // Initialize vector to contain the RK 4 step
      std::vector<Tensor<1, 3>> last_velocity;
      std::vector<Point<dim>>   last_position;
      std::vector<Tensor<1, 3>> last_omega;
      std::vector<Tensor<2, 3>> last_orientation_matrix;
      auto                      last_pp_contact_info = pp_contact_map;
      auto                      last_pw_contact_info = pw_contact_map;

      last_velocity           = std::vector<Tensor<1, 3>>(dem_particles.size());
      last_position           = std::vector<Point<dim>>(dem_particles.size());
      last_omega              = std::vector<Tensor<1, 3>>(dem_particles.size());
      last_orientation_matrix = std::vector<Tensor<2, 3>>(dem_particles.size());

      std::vector<std::vector<Tensor<1, 3>>> k_velocity =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, dim>>> k_position =
        std::vector<std::vector<Tensor<1, dim>>>(
          dem_particles.size(), std::vector<Tensor<1, dim>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_omega =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_orientation =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_impulsion =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_omega_impulsion =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_contact_impulsion =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));
      std::vector<std::vector<Tensor<1, 3>>> k_omega_contact_impulsion =
        std::vector<std::vector<Tensor<1, 3>>>(dem_particles.size(),
                                               std::vector<Tensor<1, 3>>(4));

      // Solve each of the 4 step of the RK4 method
      for (unsigned int step = 0; step < 4; ++step)
        {
          std::fill(current_fluid_force.begin(), current_fluid_force.end(), 0);
          std::fill(current_fluid_torque.begin(),
                    current_fluid_torque.end(),
                    0);
          std::fill(contact_torque.begin(), contact_torque.end(), 0);
          std::fill(contact_force.begin(), contact_force.end(), 0);
          std::fill(contact_wall_force.begin(), contact_wall_force.end(), 0);
          std::fill(contact_wall_torque.begin(), contact_wall_torque.end(), 0);
          std::fill(lubrication_force.begin(), lubrication_force.end(), 0);
          std::fill(lubrication_torque.begin(), lubrication_torque.end(), 0);
          std::fill(lubrication_wall_force.begin(),
                    lubrication_wall_force.end(),
                    0);
          std::fill(lubrication_wall_torque.begin(),
                    lubrication_wall_torque.end(),
                    0);

          // define local time of the rk step
          double local_dt = dt_dem * 0.5;
          if (step == 3)
            {
              local_dt = local_dt * 2;
            }
          // Calculate particle-particle and particle-wall contact force

          calculate_pp_contact_force(local_dt, contact_force, contact_torque);
          calculate_pw_contact_force(local_dt,
                                     contact_wall_force,
                                     contact_wall_torque);

          if (parameters->enable_lubrication_force)
            {
              calculate_pp_lubrication_force(local_dt,
                                             h_max,
                                             h_min,
                                             mu,
                                             lubrication_force,
                                             lubrication_torque);
              calculate_pw_lubrication_force(local_dt,
                                             h_max,
                                             h_min,
                                             mu,
                                             lubrication_wall_force,
                                             lubrication_wall_torque);
            }

          for (unsigned int p_i = 0; p_i < dem_particles.size(); ++p_i)
            {
              if (dem_particles[p_i].integrate_motion)
                {
                  if (step == 0)
                    {
                      last_velocity[p_i] = dem_particles[p_i].velocity;
                      last_position[p_i] = dem_particles[p_i].position;
                      last_omega[p_i]    = dem_particles[p_i].omega;
                      last_orientation_matrix[p_i] =
                        dem_particles[p_i].rotation_matrix;
                    }

                  gravity = g * (dem_particles[p_i].mass -
                                 dem_particles[p_i].volume * rho);

                  // We consider only the force at t+dt so the scheme is
                  // consistent to a BDFn scheme on the fluid side. If there is
                  // no contact.

                  current_fluid_force[p_i]  = dem_particles[p_i].fluid_forces;
                  current_fluid_torque[p_i] = dem_particles[p_i].fluid_torque;

                  // Store each rk step of the variable we integrate in its
                  // respective vector.

                  k_velocity[p_i][step] =
                    (current_fluid_force[p_i] + contact_force[p_i] +
                     contact_wall_force[p_i] + gravity +
                     lubrication_force[p_i] + lubrication_wall_force[p_i]) /
                    dem_particles[p_i].mass;

                  for (unsigned int d = 0; d < dim; ++d)
                    {
                      k_position[p_i][step][d] = dem_particles[p_i].velocity[d];
                    }
                  k_impulsion[p_i][step] =
                    current_fluid_force[p_i] + gravity +
                    contact_wall_force[p_i] + contact_force[p_i] +
                    lubrication_force[p_i] + lubrication_wall_force[p_i];

                  k_contact_impulsion[p_i][step] =
                    contact_wall_force[p_i] + contact_force[p_i] +
                    lubrication_force[p_i] + lubrication_wall_force[p_i];

                  dem_particles[p_i].velocity =
                    last_velocity[p_i] + k_velocity[p_i][step] * local_dt;

                  for (unsigned int d = 0; d < dim; ++d)
                    {
                      dem_particles[p_i].position[d] =
                        last_position[p_i][d] +
                        k_position[p_i][step][d] * local_dt;
                    }
                  dem_particles[p_i].set_position(dem_particles[p_i].position);

                  // Calculate torque in particle frame of reference.
                  Tensor<1, 3> total_torque =
                    dem_particles[p_i].rotation_matrix *
                    (current_fluid_torque[p_i] + contact_torque[p_i] +
                     contact_wall_torque[p_i]);

                  // Calculate angular acceleration in particle frame
                  Tensor<1, 3> angular_velocity_in_particle_frame =
                    dem_particles[p_i].rotation_matrix *
                    dem_particles[p_i].omega;
                  Tensor<1, 3> angular_acceleration_in_particle_frame;
                  angular_acceleration_in_particle_frame[0] =
                    (total_torque[0] -
                     (dem_particles[p_i].inertia[2][2] -
                      dem_particles[p_i].inertia[1][1]) *
                       angular_velocity_in_particle_frame[2] *
                       angular_velocity_in_particle_frame[1]) /
                    dem_particles[p_i].inertia[0][0];
                  angular_acceleration_in_particle_frame[1] =
                    (total_torque[1] -
                     (dem_particles[p_i].inertia[0][0] -
                      dem_particles[p_i].inertia[2][2]) *
                       angular_velocity_in_particle_frame[2] *
                       angular_velocity_in_particle_frame[0]) /
                    dem_particles[p_i].inertia[1][1];
                  angular_acceleration_in_particle_frame[2] =
                    (total_torque[2] -
                     (dem_particles[p_i].inertia[1][1] -
                      dem_particles[p_i].inertia[0][0]) *
                       angular_velocity_in_particle_frame[0] *
                       angular_velocity_in_particle_frame[1]) /
                    dem_particles[p_i].inertia[2][2];
                  // Rotate angular acceleration in world frame
                  k_omega[p_i][step] =
                    invert(dem_particles[p_i].rotation_matrix) *
                    angular_acceleration_in_particle_frame;

                  // Store the rk4 step of the angular contact impulsion
                  k_omega_impulsion[p_i][step] = current_fluid_torque[p_i] +
                                                 contact_torque[p_i] +
                                                 contact_wall_torque[p_i];

                  k_omega_contact_impulsion[p_i][step] =
                    contact_torque[p_i] + contact_wall_torque[p_i];
                  // Integrate the relevant state Variable for the next RK step.
                  dem_particles[p_i].omega =
                    last_omega[p_i] + k_omega[p_i][step] * local_dt;
                  k_orientation[p_i][step] = dem_particles[p_i].omega;
                  Tensor<2, 3> new_rotation_matrix;
                  if (dem_particles[p_i].omega.norm() > 0)
                    {
                      new_rotation_matrix =
                        Physics::Transformations::Rotations::rotation_matrix_3d(
                          dem_particles[p_i].omega /
                            dem_particles[p_i].omega.norm(),
                          dem_particles[p_i].omega.norm() * local_dt) *
                        last_orientation_matrix[p_i];
                      dem_particles[p_i].orientation =
                        dem_particles[p_i].shape->rotation_matrix_to_xyz_angles(
                          new_rotation_matrix);
                    }
                  // set the orientation which also update the orientation and
                  // rotation matrix of the shape.
                  dem_particles[p_i].set_orientation(
                    dem_particles[p_i].orientation);
                }
              else
                {
                  if (parameters->load_particles_from_file == false)
                    {
                      dem_particles[p_i].f_position->set_time(cfd_time + t +
                                                              local_dt);
                      dem_particles[p_i].f_velocity->set_time(cfd_time + t +
                                                              local_dt);
                      dem_particles[p_i].f_omega->set_time(cfd_time + t +
                                                           local_dt);
                      dem_particles[p_i].f_orientation->set_time(cfd_time + t +
                                                                 local_dt);

                      dem_particles[p_i].position[0] =
                        dem_particles[p_i].f_position->value(
                          dem_particles[p_i].position, 0);
                      dem_particles[p_i].position[1] =
                        dem_particles[p_i].f_position->value(
                          dem_particles[p_i].position, 1);
                      dem_particles[p_i].velocity[0] =
                        dem_particles[p_i].f_velocity->value(
                          dem_particles[p_i].position, 0);
                      dem_particles[p_i].velocity[1] =
                        dem_particles[p_i].f_velocity->value(
                          dem_particles[p_i].position, 1);
                      dem_particles[p_i].omega[0] =
                        dem_particles[p_i].f_omega->value(
                          dem_particles[p_i].position, 0);
                      dem_particles[p_i].omega[1] =
                        dem_particles[p_i].f_omega->value(
                          dem_particles[p_i].position, 1);
                      dem_particles[p_i].omega[2] =
                        dem_particles[p_i].f_omega->value(
                          dem_particles[p_i].position, 2);
                      dem_particles[p_i].orientation[0] =
                        dem_particles[p_i].f_orientation->value(
                          dem_particles[p_i].position, 0);
                      dem_particles[p_i].orientation[1] =
                        dem_particles[p_i].f_orientation->value(
                          dem_particles[p_i].position, 1);
                      dem_particles[p_i].orientation[2] =
                        dem_particles[p_i].f_orientation->value(
                          dem_particles[p_i].position, 2);
                      if (dim == 3)
                        {
                          dem_particles[p_i].position[2] =
                            dem_particles[p_i].f_position->value(
                              dem_particles[p_i].position, 2);
                          dem_particles[p_i].velocity[2] =
                            dem_particles[p_i].f_velocity->value(
                              dem_particles[p_i].position, 2);
                        }
                    }
                  else
                    {
                      dem_particles[p_i].position[0] +=
                        dem_particles[p_i].velocity[0] * dt;
                      dem_particles[p_i].position[1] +=
                        dem_particles[p_i].velocity[1] * dt;
                      dem_particles[p_i].orientation[0] =
                        dem_particles[p_i].omega[0] * dt;
                      dem_particles[p_i].orientation[1] =
                        dem_particles[p_i].omega[1] * dt;
                      dem_particles[p_i].orientation[2] =
                        dem_particles[p_i].omega[2] * dt;

                      if (dim == 3)
                        {
                          dem_particles[p_i].position[2] +=
                            dem_particles[p_i].velocity[2] * dt;
                        }
                    }
                  dem_particles[p_i].set_position(dem_particles[p_i].position);
                  dem_particles[p_i].set_orientation(
                    dem_particles[p_i].orientation);
                }
            }
          // reset the contact overlap for the next RK step.
          for (auto it = pw_contact_map.begin(); it != pw_contact_map.end();
               it++)
            {
              unsigned int particle_id = it->first;
              for (auto it2 = it->second.begin(); it2 != it->second.end();
                   it2++)
                {
                  unsigned int wall_id      = it2->first;
                  auto         contact_info = it2->second;
                  // Check if all 4 of the RK steps are in the contact info. If
                  // it is not the case, this means this is a new contact, or
                  // the contact ended. In this case, we simply take the last RK
                  // step registered as the derivative for the integration.
                  if (last_pw_contact_info.find(particle_id) !=
                      last_pw_contact_info.end())
                    {
                      auto previous_contact_info_iterator =
                        last_pw_contact_info.find(particle_id)
                          ->second.find(wall_id);
                      if (previous_contact_info_iterator !=
                          last_pw_contact_info.find(particle_id)->second.end())
                        {
                          contact_info.tangential_displacement =
                            previous_contact_info_iterator->second
                              .tangential_displacement;
                        }
                      else
                        {
                          contact_info.tangential_displacement = 0;
                        }
                    }
                  else
                    {
                      // one of the map is empty and we try to acces it.
                      contact_info.tangential_displacement = 0;
                    }
                }
            }

          for (auto it = pp_contact_map.begin(); it != pp_contact_map.end();
               it++)
            {
              unsigned int particle_one_id = it->first;
              for (auto it2 = it->second.begin(); it2 != it->second.end();
                   it2++)
                {
                  unsigned int particle_two_id = it2->first;
                  auto         contact_info    = it2->second;
                  // Check if all 4 of the RK steps are in the contact info. If
                  // it is not the case, this means this is a new contact, or
                  // the contact ended. In this case, we simply take the last RK
                  // step registered as the derivative for the integration.
                  if (last_pp_contact_info.find(particle_one_id) !=
                      last_pp_contact_info.end())
                    {
                      auto previous_contact_info_iterator =
                        last_pp_contact_info.find(particle_one_id)
                          ->second.find(particle_two_id);
                      if (previous_contact_info_iterator !=
                          last_pp_contact_info.find(particle_one_id)
                            ->second.end())
                        {
                          contact_info.tangential_displacement =
                            previous_contact_info_iterator->second
                              .tangential_displacement;
                        }
                      else
                        {
                          contact_info.tangential_displacement = 0;
                        }
                    }
                  else
                    {
                      contact_info.tangential_displacement = 0;
                    }
                }
            }
        }


      for (unsigned int p_i = 0; p_i < dem_particles.size(); ++p_i)
        {
          if (dem_particles[p_i].integrate_motion)
            {
              // Define the integral by combining each of the RK step.
              dem_particles[p_i].velocity =
                last_velocity[p_i] +
                dt_dem *
                  (k_velocity[p_i][0] + 2 * k_velocity[p_i][1] +
                   2 * k_velocity[p_i][2] + k_velocity[p_i][3]) /
                  6;

              for (unsigned int d = 0; d < dim; ++d)
                {
                  dem_particles[p_i].position[d] =
                    last_position[p_i][d] +
                    dt_dem *
                      (k_position[p_i][0][d] + 2 * k_position[p_i][1][d] +
                       2 * k_position[p_i][2][d] + k_position[p_i][3][d]) /
                      6;
                }

              dem_particles[p_i].omega =
                last_omega[p_i] + dt_dem *
                                    (k_omega[p_i][0] + 2 * k_omega[p_i][1] +
                                     2 * k_omega[p_i][2] + k_omega[p_i][3]) /
                                    6;
              Tensor<1, 3> angle_update_vector =
                (k_orientation[p_i][0] + 2 * k_orientation[p_i][1] +
                 2 * k_orientation[p_i][2] + k_orientation[p_i][3]) /
                6;
              // Update orientation matrix explicitly
              Tensor<2, 3> new_rotation_matrix;
              if (angle_update_vector.norm() > 0)
                {
                  new_rotation_matrix =
                    Physics::Transformations::Rotations::rotation_matrix_3d(
                      angle_update_vector / angle_update_vector.norm(),
                      angle_update_vector.norm() * dt_dem) *
                    last_orientation_matrix[p_i];
                  dem_particles[p_i].orientation =
                    dem_particles[p_i].shape->rotation_matrix_to_xyz_angles(
                      new_rotation_matrix);
                }

              // Integration of the impulsion applied to the particle.
              // This is what will be transferred to the CFD to integrate the
              // particle.
              dem_particles[p_i].impulsion +=
                dt_dem *
                (k_impulsion[p_i][0] + 2 * k_impulsion[p_i][1] +
                 2 * k_impulsion[p_i][2] + k_impulsion[p_i][3]) /
                6;

              dem_particles[p_i].contact_impulsion +=
                dt_dem *
                (k_contact_impulsion[p_i][0] + 2 * k_contact_impulsion[p_i][1] +
                 2 * k_contact_impulsion[p_i][2] +
                 k_contact_impulsion[p_i][3]) /
                6;

              dem_particles[p_i].omega_impulsion +=
                dt_dem *
                (k_omega_impulsion[p_i][0] + 2 * k_omega_impulsion[p_i][1] +
                 2 * k_omega_impulsion[p_i][2] + k_omega_impulsion[p_i][3]) /
                6;

              dem_particles[p_i].omega_contact_impulsion +=
                dt_dem *
                (k_omega_contact_impulsion[p_i][0] +
                 2 * k_omega_contact_impulsion[p_i][1] +
                 2 * k_omega_contact_impulsion[p_i][2] +
                 k_omega_contact_impulsion[p_i][3]) /
                6;

              // update particle position and orientation
              dem_particles[p_i].set_position(dem_particles[p_i].position);
              dem_particles[p_i].set_orientation(
                dem_particles[p_i].orientation);
            }
        }

      // RK4 for the particle wall tangential displacement
      for (auto it = pw_contact_map.begin(); it != pw_contact_map.end(); it++)
        {
          unsigned int particle_id = it->first;
          for (auto it2 = it->second.begin(); it2 != it->second.end(); it2++)
            {
              unsigned int wall_id      = it2->first;
              auto         contact_info = it2->second;
              // Check if all 4 of the RK steps are in the contact info. If it
              // is not the case, this means this is a new contact, or the
              // contact ended. In this case, we simply take the last RK step
              // registered as the derivative for the integration.
              Tensor<1, 3> previous_tangential_displacement;
              if (last_pw_contact_info.find(particle_id) !=
                  last_pw_contact_info.end())
                {
                  auto previous_contact_info_iterator =
                    last_pw_contact_info.find(particle_id)
                      ->second.find(wall_id);
                  if (previous_contact_info_iterator !=
                      last_pw_contact_info.find(particle_id)->second.end())
                    {
                      previous_tangential_displacement =
                        previous_contact_info_iterator->second
                          .tangential_displacement;
                    }
                }
              else
                {
                  previous_tangential_displacement = 0;
                }
              if (contact_info.tangential_relative_velocity.size() == 4)
                {
                  contact_info.tangential_displacement =
                    previous_tangential_displacement +
                    dt_dem / 6.0 *
                      (contact_info.tangential_relative_velocity[0] +
                       2 * contact_info.tangential_relative_velocity[1] +
                       2 * contact_info.tangential_relative_velocity[2] +
                       contact_info.tangential_relative_velocity[3]);
                }
              else if (contact_info.tangential_relative_velocity.size() > 0)
                {
                  contact_info.tangential_displacement =
                    previous_tangential_displacement +
                    dt_dem *
                      contact_info.tangential_relative_velocity
                        [contact_info.tangential_relative_velocity.size()];
                }
              else
                {
                  contact_info.tangential_displacement = 0;
                }


              // clear the vector for the next set of 4 Rk steps
              contact_info.tangential_relative_velocity.clear();
            }
        }
      // Same for particle-particle tangential displacement
      for (auto it = pp_contact_map.begin(); it != pp_contact_map.end(); it++)
        {
          unsigned int particle_one_id = it->first;
          for (auto it2 = it->second.begin(); it2 != it->second.end(); it2++)
            {
              unsigned int particle_two_id = it2->first;
              auto         contact_info    = it2->second;
              // Check if all 4 of the RK steps are in the contact info. If it
              // is not the case, this means this is a new contact, or the
              // contact ended. In this case, we simply take the last RK step
              // registered as the derivative for the integration.
              Tensor<1, 3> previous_tangential_displacement;
              if (last_pp_contact_info.find(particle_one_id) !=
                  last_pp_contact_info.end())
                {
                  auto previous_contact_info_iterator =
                    last_pp_contact_info.find(particle_one_id)
                      ->second.find(particle_two_id);
                  if (previous_contact_info_iterator !=
                      last_pp_contact_info.find(particle_one_id)->second.end())
                    {
                      previous_tangential_displacement =
                        previous_contact_info_iterator->second
                          .tangential_displacement;
                    }
                }
              else
                {
                  previous_tangential_displacement = 0;
                }
              if (contact_info.tangential_relative_velocity.size() == 4)
                {
                  contact_info.tangential_displacement =
                    previous_tangential_displacement +
                    dt_dem / 6.0 *
                      (contact_info.tangential_relative_velocity[0] +
                       2 * contact_info.tangential_relative_velocity[1] +
                       2 * contact_info.tangential_relative_velocity[2] +
                       contact_info.tangential_relative_velocity[3]);
                }
              else if (contact_info.tangential_relative_velocity.size() > 0)
                {
                  contact_info.tangential_displacement =
                    previous_tangential_displacement +
                    dt_dem *
                      contact_info.tangential_relative_velocity
                        [contact_info.tangential_relative_velocity.size()];
                }
              else
                {
                  contact_info.tangential_displacement = 0;
                }
              // clear the vector for the next set of 4 Rk steps
              contact_info.tangential_relative_velocity.clear();
            }
        }
      t += dt_dem;
    }
}



template class IBParticlesDEM<2>;
template class IBParticlesDEM<3>;
