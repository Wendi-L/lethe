# SPDX-FileCopyrightText: Copyright (c) 2024-2025 The Lethe Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception OR LGPL-2.1-or-later

# Listing of Parameters
# ---------------------

set dimension = 2

#---------------------------------------------------
# Simulation and IO Control
#---------------------------------------------------

subsection simulation control
  set method           = bdf2
  set time end         = 0.08
  set time step        = 4.4e-5
  set adapt            = true
  set max cfl          = 0.75
  set max time step    = 4.4e-5
  set output name      = rayleigh-plateau
  set output frequency = 5
  set output path      = ./output_deltaDELTA_VALUE_OUTPUT/
end

#---------------------------------------------------
# Multiphysics
#---------------------------------------------------

subsection multiphysics
  set VOF = true
end

#---------------------------------------------------
# VOF
#---------------------------------------------------

subsection VOF
  subsection surface tension force
    set enable                  = true
    set output auxiliary fields = true
  end
  subsection interface regularization method
    set type      = projection-based interface sharpening
    set frequency = 20
    subsection projection-based interface sharpening
      set interface sharpness = 1.5
    end
  end
  subsection phase filtration
    set type = tanh
    set beta = 10
  end
end

#---------------------------------------------------
# Initial condition
#---------------------------------------------------

subsection initial conditions
  set type = nodal
  subsection uvwp
    set Function constants  = U=1.569
    set Function expression = if(y^2 <= 1.3110e-6, U, 0); 0; 0
  end
  subsection VOF
    set Function expression = if(y^2 <= 1.3110e-6, 1, 0)
    subsection projection step
      set enable = true
    end
  end
end

#---------------------------------------------------
# Physical Properties
#---------------------------------------------------

subsection physical properties
  set number of fluids = 2
  subsection fluid 0
    set density             = 1.196
    set kinematic viscosity = 2.54e-4
  end
  subsection fluid 1
    set density             = 1196
    set kinematic viscosity = 2.54e-5
  end
  set number of material interactions = 1
  subsection material interaction 0
    set type = fluid-fluid
    subsection fluid-fluid interaction
      set surface tension coefficient = 0.0674
    end
  end
end

#---------------------------------------------------
# Mesh
#---------------------------------------------------

subsection mesh
  set type               = dealii
  set grid type          = subdivided_hyper_rectangle
  set grid arguments     = 4 , 1 : 0, -0.01145 : 0.0916, 0.01145 : true
  set initial refinement = 7
end

#---------------------------------------------------
# Mesh Adaptation
#---------------------------------------------------

subsection mesh adaptation
  set type                     = kelly
  set variable                 = phase
  set fraction type            = fraction
  set max refinement level     = 8
  set min refinement level     = 5
  set fraction refinement      = 0.99
  set fraction coarsening      = 0.001
  set initial refinement steps = 4
end

# --------------------------------------------------
# Boundary Conditions
#---------------------------------------------------

subsection boundary conditions
  set number = 3
  subsection bc 0
    set id   = 0
    set type = function
    subsection u
      set Function constants  = U=1.569, delta=DELTA_VALUE, kappa=0.7, r=1.145e-3
      set Function expression = if (y^2 <= 1.3110e-6, U*(1 + delta*sin(kappa*U*t/r)), 0)
    end
  end
  subsection bc 1
    set id                 = 2
    set type               = periodic
    set periodic_id        = 3
    set periodic_direction = 1
  end
  subsection bc 2
    set id                 = 1
    set type               = outlet
    set beta               = 0
  end
end

# --------------------------------------------------
# Boundary Conditions VOF
#---------------------------------------------------

subsection boundary conditions VOF
  set number = 4
  subsection bc 0
    set id   = 0
    set type = dirichlet
    subsection dirichlet
      set Function expression = if(y^2 <= 1.3110e-6, 1, 0)
    end
  end
end

# --------------------------------------------------
# Non-Linear Solver Control
#---------------------------------------------------

subsection non-linear solver
  subsection fluid dynamics
    set tolerance      = 1e-6
    set max iterations = 20
  end
  subsection VOF
    set tolerance      = 1e-10
    set max iterations = 2
  end
end

# --------------------------------------------------
# Linear Solver Control
#---------------------------------------------------

subsection linear solver
  subsection fluid dynamics
    set relative residual  = 1e-3
    set minimum residual   = 1e-9
    set preconditioner     = amg
  end
  subsection VOF
    set relative residual = 1e-8
    set minimum residual  = 1e-11
  end
end

# --------------------------------------------------
# Timer
#---------------------------------------------------

subsection timer
  set type = iteration
end

# --------------------------------------------------
# Restart
#---------------------------------------------------

subsection restart
  set checkpoint = true
  set frequency  = 50
  set filename   = restart
  set restart    = false
end
