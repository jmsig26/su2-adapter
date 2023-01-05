/*!
 * \file driver_direct_singlezone.cpp
 * \brief The main subroutines for driving single-zone problems.
 * \author R. Sanchez
 * \version 7.4.0 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2022, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../include/drivers/CSinglezoneDriver.hpp"

#include "../../include/definition_structure.hpp"
#include "../../include/iteration/CIteration.hpp"
#include "../../include/output/COutput.hpp"
#include "../../include/precice.hpp"

CSinglezoneDriver::CSinglezoneDriver(char* confFile, unsigned short val_nZone, SU2_Comm MPICommunicator)
    : CDriver(confFile, val_nZone, MPICommunicator, false) {
  /*--- Initialize the counter for TimeIter ---*/
  TimeIter = 0;
}

CSinglezoneDriver::~CSinglezoneDriver(void) {}

void CSinglezoneDriver::StartSolver() {
  StartTime = SU2_MPI::Wtime();

  config_container[ZONE_0]->Set_StartTime(StartTime);

  // preCICE
  precice_usage = config_container[ZONE_0]->GetpreCICE_Usage();
  if (precice_usage) {
    precice = new Precice(
        config_container[ZONE_0]->GetpreCICE_ConfigFileName(), config_container[ZONE_0]->GetpreCICE_ParticipantName(),
        config_container[ZONE_0]->GetpreCICE_ReadDataName(), config_container[ZONE_0]->GetpreCICE_WriteDataName(),
        config_container[ZONE_0]->GetpreCICE_MeshName(), rank, size, geometry_container, solver_container,
        config_container, grid_movement);
    dt = new double(config_container[ZONE_0]->GetDelta_UnstTime());
    max_precice_dt = new double(precice->initialize());

    /* Implemented continuous adjoint check in precice class instead
    // Saving GridVel_Grad is only important when using Continuous Adjoint
    // SU2 no longer even instantiates GridVel_Grad when not using it
    // This would break the adapter -- so instantiate it if it hasn't been already
    if (geometry_container[ZONE_0][INST_0][MESH_0]->nodes->GetGridVel_Grad().size() < 1)
    {
            int nDim = geometry_container[ZONE_0][INST_0][MESH_0]->GetnDim();
            unsigned long nPoint = geometry_container[ZONE_0][INST_0][MESH_0]->GetnPoint();
            (geometry_container[ZONE_0][INST_0][MESH_0]->nodes->GetGridVel_Grad()).resize(nPoint, nDim, nDim, 0.0);
            // Create GridVel_Grad w/ just a bunch of zeros
    }
    */
  }
  /*--- Main external loop of the solver. Runs for the number of time steps required. ---*/

  if (rank == MASTER_NODE)
    cout << endl << "------------------------------ Begin Solver -----------------------------" << endl;

  if (rank == MASTER_NODE) {
    cout << endl << "Simulation Run using the Single-zone Driver" << endl;
    if (driver_config->GetTime_Domain())
      cout << "The simulation will run for "
           << driver_config->GetnTime_Iter() - config_container[ZONE_0]->GetRestart_Iter() << " time steps." << endl;
  }

  /*--- Set the initial time iteration to the restart iteration. ---*/
  if (config_container[ZONE_0]->GetRestart() && driver_config->GetTime_Domain())
    TimeIter = config_container[ZONE_0]->GetRestart_Iter();

  /*--- Run the problem until the number of time iterations required is reached. ---*/
  // preCICE
  while ((precice_usage && precice->isCouplingOngoing()) ||
         (TimeIter < config_container[ZONE_0]->GetnTime_Iter() && !precice_usage)) {
    // preCICE implicit coupling: saveOldState()
    if (precice_usage && precice->isActionRequired(precice->getCowic())) {
      precice->saveOldState(&StopCalc, dt);
    }

    // preCICE - set minimal time step size as new time step size in SU2
    if (precice_usage) {
      dt = min(max_precice_dt, dt);
      config_container[ZONE_0]->SetDelta_UnstTimeND((*dt) / config_container[ZONE_0]->GetTime_Ref());
    }

    /*--- Perform some preprocessing before starting the time-step simulation. ---*/
    Preprocess(TimeIter);

    /*--- Run a time-step iteration of the single-zone problem. ---*/
    Run();

    /*--- Perform some postprocessing on the solution before the update ---*/
    cout << "Completed Run. Running Postprocess()" << endl;
    Postprocess();

    /*--- Update the solution for dual time stepping strategy ---*/
    Update();

    /*--- Monitor the computations after each iteration. ---*/
    Monitor(TimeIter);

    // preCICE - Advancing
    if (precice_usage) {
      *max_precice_dt = precice->advance(*dt);
    }

    /*--- Output the solution in files. ---*/

    // preCICE implicit coupling: reloadOldState()
    bool suppress_output_by_preCICE = false;
    if (precice_usage && precice->isActionRequired(precice->getCoric())) {
      // Stay at the same iteration number if preCICE is not converged and reload to the state before the current
      // iteration
      TimeIter--;
      precice->reloadOldState(&StopCalc, dt);
      suppress_output_by_preCICE = true;
    }

    // preCICE
    Output(TimeIter, suppress_output_by_preCICE);

    /*--- Save iteration solution for libROM ---*/
    if (config_container[MESH_0]->GetSave_libROM()) {
      solver_container[ZONE_0][INST_0][MESH_0][FLOW_SOL]->SavelibROM(geometry_container[ZONE_0][INST_0][MESH_0],
                                                                     config_container[ZONE_0], StopCalc);
    }

    /*--- If the convergence criteria has been met, terminate the simulation. ---*/
    // preCICE
    if (StopCalc && !precice_usage) break;

    TimeIter++;
  }
}

void CSinglezoneDriver::Preprocess(unsigned long TimeIter) {
  /*--- Set runtime option ---*/

  Runtime_Options();

  /*--- Set the current time iteration in the config ---*/

  config_container[ZONE_0]->SetTimeIter(TimeIter);

  /*--- Store the current physical time in the config container, as
   this can be used for verification / MMS. This should also be more
   general once the drivers are more stable. ---*/

  if (config_container[ZONE_0]->GetTime_Marching() != TIME_MARCHING::STEADY)
    config_container[ZONE_0]->SetPhysicalTime(static_cast<su2double>(TimeIter) *
                                              config_container[ZONE_0]->GetDelta_UnstTimeND());
  else
    config_container[ZONE_0]->SetPhysicalTime(0.0);

  /*--- Set the initial condition for EULER/N-S/RANS ---------------------------------------------*/
  if (config_container[ZONE_0]->GetFluidProblem()) {
    solver_container[ZONE_0][INST_0][MESH_0][FLOW_SOL]->SetInitialCondition(
        geometry_container[ZONE_0][INST_0], solver_container[ZONE_0][INST_0], config_container[ZONE_0], TimeIter);
  } else if (config_container[ZONE_0]->GetHeatProblem()) {
    /*--- Set the initial condition for HEAT equation ---------------------------------------------*/
    solver_container[ZONE_0][INST_0][MESH_0][HEAT_SOL]->SetInitialCondition(
        geometry_container[ZONE_0][INST_0], solver_container[ZONE_0][INST_0], config_container[ZONE_0], TimeIter);
  }

  SU2_MPI::Barrier(SU2_MPI::GetComm());

  /*--- Run a predictor step ---*/
  if (config_container[ZONE_0]->GetPredictor())
    iteration_container[ZONE_0][INST_0]->Predictor(output_container[ZONE_0], integration_container, geometry_container,
                                                   solver_container, numerics_container, config_container,
                                                   surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- Perform a dynamic mesh update if required. ---*/
  /*--- For the Disc.Adj. of a case with (rigidly) moving grid, the appropriate
          mesh cordinates are read from the restart files. ---*/
  if (!(config_container[ZONE_0]->GetGrid_Movement() && config_container[ZONE_0]->GetDiscrete_Adjoint()))
    DynamicMeshUpdate(TimeIter);
}

void CSinglezoneDriver::Run() {
  unsigned long OuterIter = 0;
  config_container[ZONE_0]->SetOuterIter(OuterIter);

  /*--- Iterate the zone as a block, either to convergence or to a max number of iterations ---*/
  iteration_container[ZONE_0][INST_0]->Solve(output_container[ZONE_0], integration_container, geometry_container,
                                             solver_container, numerics_container, config_container, surface_movement,
                                             grid_movement, FFDBox, ZONE_0, INST_0);
}

void CSinglezoneDriver::Postprocess() {
  iteration_container[ZONE_0][INST_0]->Postprocess(output_container[ZONE_0], integration_container, geometry_container,
                                                   solver_container, numerics_container, config_container,
                                                   surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- A corrector step can help preventing numerical instabilities ---*/

  if (config_container[ZONE_0]->GetRelaxation())
    iteration_container[ZONE_0][INST_0]->Relaxation(output_container[ZONE_0], integration_container, geometry_container,
                                                    solver_container, numerics_container, config_container,
                                                    surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);
}

void CSinglezoneDriver::Update() {
  iteration_container[ZONE_0][INST_0]->Update(output_container[ZONE_0], integration_container, geometry_container,
                                              solver_container, numerics_container, config_container, surface_movement,
                                              grid_movement, FFDBox, ZONE_0, INST_0);
}

void CSinglezoneDriver::Output(unsigned long TimeIter, bool suppress_output_by_preCICE) {
  /*--- Time the output for performance benchmarking. ---*/

  StopTime = SU2_MPI::Wtime();

  UsedTimeCompute += StopTime - StartTime;

  StartTime = SU2_MPI::Wtime();

  // preCICE: Output solution only, if preCICE converged; otherwise suppress output
  bool wrote_files = false;
  if (precice_usage && !suppress_output_by_preCICE)
    bool wrote_files =
        output_container[ZONE_0]->SetResult_Files(geometry_container[ZONE_0][INST_0][MESH_0], config_container[ZONE_0],
                                                  solver_container[ZONE_0][INST_0][MESH_0], TimeIter, StopCalc);

  if (wrote_files) {
    StopTime = SU2_MPI::Wtime();

    UsedTimeOutput += StopTime - StartTime;
    OutputCount++;
    BandwidthSum = config_container[ZONE_0]->GetRestart_Bandwidth_Agg();

    StartTime = SU2_MPI::Wtime();
  }

  config_container[ZONE_0]->Set_StartTime(StartTime);
}

void CSinglezoneDriver::DynamicMeshUpdate(unsigned long TimeIter) {
  auto iteration = iteration_container[ZONE_0][INST_0];

  /*--- Legacy dynamic mesh update - Only if GRID_MOVEMENT = YES ---*/
  if (config_container[ZONE_0]->GetGrid_Movement()) {
    iteration->SetGrid_Movement(geometry_container[ZONE_0][INST_0], surface_movement[ZONE_0],
                                grid_movement[ZONE_0][INST_0], solver_container[ZONE_0][INST_0],
                                config_container[ZONE_0], 0, TimeIter);
  }

  /*--- New solver - all the other routines in SetGrid_Movement should be adapted to this one ---*/
  /*--- Works if DEFORM_MESH = YES ---*/
  iteration->SetMesh_Deformation(geometry_container[ZONE_0][INST_0], solver_container[ZONE_0][INST_0][MESH_0],
                                 numerics_container[ZONE_0][INST_0][MESH_0], config_container[ZONE_0],
                                 RECORDING::CLEAR_INDICES);

  /*--- Update the wall distances if the mesh was deformed. ---*/
  if (config_container[ZONE_0]->GetGrid_Movement() || config_container[ZONE_0]->GetDeform_Mesh()) {
    CGeometry::ComputeWallDistance(config_container, geometry_container);
  }
}

bool CSinglezoneDriver::Monitor(unsigned long TimeIter) {
  unsigned long nInnerIter, InnerIter, nTimeIter;
  su2double MaxTime, CurTime;
  bool TimeDomain, InnerConvergence, TimeConvergence, FinalTimeReached, MaxIterationsReached;

  nInnerIter = config_container[ZONE_0]->GetnInner_Iter();
  InnerIter = config_container[ZONE_0]->GetInnerIter();
  nTimeIter = config_container[ZONE_0]->GetnTime_Iter();
  MaxTime = config_container[ZONE_0]->GetMax_Time();
  CurTime = output_container[ZONE_0]->GetHistoryFieldValue("CUR_TIME");

  TimeDomain = config_container[ZONE_0]->GetTime_Domain();

  /*--- Check whether the inner solver has converged --- */

  if (TimeDomain == NO) {
    InnerConvergence = output_container[ZONE_0]->GetConvergence();
    MaxIterationsReached = InnerIter + 1 >= nInnerIter;

    if ((MaxIterationsReached || InnerConvergence) && (rank == MASTER_NODE)) {
      cout << endl << "----------------------------- Solver Exit -------------------------------" << endl;
      if (InnerConvergence)
        cout << "All convergence criteria satisfied." << endl;
      else
        cout << endl
             << "Maximum number of iterations reached (ITER = " << nInnerIter << ") before convergence." << endl;
      output_container[ZONE_0]->PrintConvergenceSummary();
      cout << "-------------------------------------------------------------------------" << endl;
    }

    StopCalc = MaxIterationsReached || InnerConvergence;
  }

  if (TimeDomain == YES) {
    /*--- Check whether the outer time integration has reached the final time ---*/

    TimeConvergence = GetTimeConvergence();

    FinalTimeReached = CurTime >= MaxTime;
    MaxIterationsReached = TimeIter + 1 >= nTimeIter;

    if ((FinalTimeReached || MaxIterationsReached || TimeConvergence) && (rank == MASTER_NODE)) {
      cout << endl << "----------------------------- Solver Exit -------------------------------";
      if (TimeConvergence) cout << endl << "All windowed time-averaged convergence criteria are fullfilled." << endl;
      if (FinalTimeReached) cout << endl << "Maximum time reached (MAX_TIME = " << MaxTime << "s)." << endl;
      if (MaxIterationsReached)
        cout << endl << "Maximum number of time iterations reached (TIME_ITER = " << nTimeIter << ")." << endl;
      cout << "-------------------------------------------------------------------------" << endl;
    }
    StopCalc = FinalTimeReached || MaxIterationsReached || TimeConvergence;
  }

  /*--- Reset the inner convergence --- */

  output_container[ZONE_0]->SetConvergence(false);

  /*--- Increase the total iteration count --- */

  IterCount += config_container[ZONE_0]->GetInnerIter() + 1;

  return StopCalc;
}

void CSinglezoneDriver::Runtime_Options() {
  ifstream runtime_configfile;

  /*--- Try to open the runtime config file ---*/

  runtime_configfile.open(runtime_file_name, ios::in);

  /*--- If succeeded create a temporary config object ---*/

  if (runtime_configfile.good()) {
    CConfig* runtime = new CConfig(runtime_file_name, config_container[ZONE_0]);
    delete runtime;
  }
}

bool CSinglezoneDriver::GetTimeConvergence() const {
  return output_container[ZONE_0]->GetCauchyCorrectedTimeConvergence(config_container[ZONE_0]);
}

// preCICE: implementation of updated Postprocessing fxn to deallocate preCICE vars
void CSinglezoneDriver::Postprocessing() {
  // preCICE - Finalize
  if (precice_usage) {
    precice->finalize();
    if (dt != NULL) {
      delete dt;
    }
    if (max_precice_dt != NULL) {
      delete max_precice_dt;
    }
    if (precice != NULL) {
      delete precice;
    }
  }

  // Call super::Postprocessing()
  CDriver::Postprocessing();
}