/************************************************************************
	
	Copyright 2007-2009 Emre Sozer & Patrick Clark Trizila

	Contact: emresozer@freecfd.com , ptrizila@freecfd.com

	This file is a part of Free CFD

	Free CFD is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    any later version.

    Free CFD is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    For a copy of the GNU General Public License,
    see <http://www.gnu.org/licenses/>.

*************************************************************************/
#define DEBUG 0

#include <iostream>
#include <fstream>
#include <map>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <limits>

#include "commons.h"
#include "inputs.h"
#include "bc.h"
#include "mpi_functions.h"
#include "petsc_functions.h"
#include "probe.h"
using namespace std;

// Function prototypes
void read_inputs(InputFile &input);
void initialize(InputFile &input);
void write_output(double time, InputFile input);
void write_restart(double time);
void read_restart(double &time);
void setBCs(InputFile &input, BC &bc);
bool withinBox(Vec3D point,Vec3D corner_1,Vec3D corner_2);
bool withinCylinder(Vec3D point,Vec3D center,double radius,Vec3D axisDirection,double height);
bool withinSphere(Vec3D point,Vec3D center,double radius);
double minmod(double a, double b);
double maxmod(double a, double b);
double doubleMinmod(double a, double b);
double harmonic(double a, double b);
double superbee(double a, double b);
void update(void);
void update_turb(void);
string int2str(int number) ;
void assemble_linear_system(void);
void initialize_linear_system(void);
void update_face_mdot(void);
void linear_system_turb(void);
void update_eddy_viscosity(void);
void get_dt(void);
void get_CFLmax(void);
void set_probes(void);
void set_integrateBoundary(void);
void set_turbulence_model_constants(void);
inline double signof(double a) { return (a == 0.) ? 0. : (a<0. ? -1. : 1.); }
double temp;

// Allocate container for boundary conditions
BC bc;
InputFile input;

vector<Probe> probes;
vector<BoundaryFlux> boundaryFluxes;

double resP,resV,resT,resK,resOmega;

int main(int argc, char *argv[]) {

	// Set global parameters
	sqrt_machine_error=sqrt(std::numeric_limits<double>::epsilon());
	
	// Initialize mpi
	mpi_init(argc,argv);

	string inputFileName, gridFileName;
	inputFileName.assign(argv[1]);
	gridFileName.assign(argv[1]);
	inputFileName+=".in";
	gridFileName+=".cgns";
	//gridFileName+=".dat";

	restart=0;
	if (argc>2) restart=atoi(argv[2]);

	input.setFile(inputFileName);
	read_inputs(input);
	// Set equation of state based on the inputs
	eos.set();

	// Physical time
	double time=0.;

	if (restart!=0 && ramp) {
		ramp=false;
	}
	int timeStepMax=input.section("timeMarching").get_int("numberOfSteps");
	
	grid.read(gridFileName);
	// Hand shake with other processors
	// (agree on which cells' data to be shared)
	mpi_handshake();
	mpi_get_ghost_centroids();
	
	initialize(input);
	if (Rank==0) cout << "[I] Applied initial conditions" << endl;

	setBCs(input,bc);
	if (Rank==0) cout << "[I] Set boundary conditions" << endl;

	set_probes();
	set_integrateBoundary();

	grid.lengthScales();
	
	if (Rank==0) cout << "[I] Calculating node averaging metrics, this might take a while..." << endl;
	grid.nodeAverages(); // Linear triangular (tetrahedral) + idw blended mode
	//grid.nodeAverages_idw(); // Inverse distance based mode
	grid.faceAverages();
	grid.gradMaps();
	
	set_turbulence_model_constants();
			
	if (restart!=0) {
		for (int p=0;p<np;++p) {
			if (Rank==p) read_restart(time);
			MPI_Barrier(MPI_COMM_WORLD);
		}
	}

	// Initialize petsc
	petsc_init(argc,argv,
				input.section("linearSolver").get_double("relTolerance"),
				input.section("linearSolver").get_double("absTolerance"),
				input.section("linearSolver").get_int("maxIterations"));
	
	// Initialize time step or CFL size if ramping
	if (ramp) {
		if (TIME_STEP_TYPE==FIXED) dt=ramp_initial;
		if (TIME_STEP_TYPE==CFL_MAX) CFLmax=ramp_initial;
		if (TIME_STEP_TYPE==CFL_LOCAL) CFLlocal=ramp_initial;
	}

	if (Rank==0) cout << "[I] Beginning time loop" << endl;
	// Begin timing
	MPI_Barrier(MPI_COMM_WORLD);
	double timeRef, timeEnd;
	timeRef=MPI_Wtime();

	/*****************************************************************************************/
	// Begin time loop
	/*****************************************************************************************/
	bool firstTimeStep=true;
	for (timeStep=restart+1;timeStep<=timeStepMax+restart;++timeStep) {

		int nIter,nIterTurb; // Number of linear solver iterations
		double rNorm,rNormTurb; // Residual norm of the linear solver

		// Flush boundary forces
		if ((timeStep) % integrateBoundaryFreq == 0) {
			for (int i=0;i<bcCount;++i) {
				bc.region[i].mass=0.;
				bc.region[i].momentum=0.;
				bc.region[i].energy=0.;
			}
		}
		
		if (firstTimeStep) {
			mpi_update_ghost_primitives();
			// Gradient are calculated for NS and/or second order schemes
			if (EQUATIONS==NS | order==SECOND ) {
			// Calculate all the cell gradients for each variable
				grid.gradients();
			// Update gradients of the ghost cells
				mpi_update_ghost_gradients();
			}
		
			if (LIMITER!=NONE) {
			// Limit gradients
				grid.limit_gradients(); 
			// Update limited gradients of the ghost cells
				mpi_update_ghost_gradients();
			}
		}


		// Get time step (will handle fixed, CFL and CFLramp)
		get_dt();
		if (TIME_STEP_TYPE==FIXED) { get_CFLmax(); } 
		else if (TIME_STEP_TYPE==CFL_LOCAL) { CFLmax=CFLlocal;}
		
		if (TURBULENCE_MODEL!=NONE) {
			if (firstTimeStep) {
				mpi_update_ghost_turb();
				update_eddy_viscosity();
			} else {
				grid.gradients_turb();
				mpi_update_ghost_gradients_turb();
				grid.limit_gradients_turb();
				mpi_update_ghost_gradients_turb();
				update_face_mdot();
				linear_system_turb();
				petsc_solve_turb(nIterTurb,rNormTurb);
				update_turb();
				mpi_update_ghost_turb();
				update_eddy_viscosity();
			}
		}
		
		initialize_linear_system();
		assemble_linear_system();
		petsc_solve(nIter,rNorm);
		update();
		mpi_update_ghost_primitives();
		// Gradient are calculated for NS and/or second order schemes
		if (EQUATIONS==NS | order==SECOND ) {
			// Calculate all the cell gradients for each variable
			grid.gradients();
			// Update gradients of the ghost cells
			mpi_update_ghost_gradients();
		}
		
		if (LIMITER!=NONE) {
			// Limit gradients
			grid.limit_gradients(); 
			// Update limited gradients of the ghost cells
			mpi_update_ghost_gradients();
		}

		// Advance physical time
		time += dt;
		if (Rank==0) {
			if (timeStep==(restart+1))  cout << "step" << "\t" << "time" << "\t\t" << "dt" << "\t\t" << "CFLmax" << "\t\t" << "nIter" << "\t" << "LinearRes" << "\t" << "pRes" << "\t\t" << "vRes" << "\t\t" << "TRes" << endl;
			cout << timeStep << "\t" << setprecision(4) << scientific << time << "\t" << dt << "\t" << CFLmax << "\t" << nIter << "\t" << rNorm << "\t" << resP << "\t" << resV << "\t" << resT << "\t" << resK << "\t" << resOmega << endl;
			if (!firstTimeStep) cout << nIterTurb << "\t" << rNormTurb << endl;
		}
		
		// Ramp-up if needed
		if (ramp) {
			if (TIME_STEP_TYPE==FIXED) dt=min(dt*ramp_growth,dtTarget);
			if (TIME_STEP_TYPE==CFL_MAX) CFLmax=min(CFLmax*ramp_growth,CFLmaxTarget);
			if (TIME_STEP_TYPE==CFL_LOCAL) CFLlocal=min(CFLlocal*ramp_growth,CFLlocalTarget);
		}
			
		if ((timeStep) % restartFreq == 0) {
			for (int p=0;p<np;p++) {
				if (p==Rank) write_restart(time);
				// Syncronize the processors
				MPI_Barrier(MPI_COMM_WORLD);
			}
		}

		if ((timeStep) % outFreq == 0) write_output(time,input);
		
		if ((timeStep) % probeFreq == 0) {

			for (int p=0;p<probes.size();++p) {
				Cell &c=grid.cell[probes[p].nearestCell];
				ofstream file;
				file.open((probes[p].fileName).c_str(),ios::app);
				file << timeStep << "\t" << setw(16) << setprecision(8)  << time << "\t" << c.p << "\t" << c.v[0] << "\t" << c.v[1] << "\t" << c.v[2] << "\t" << c.T << "\t" << c.rho << "\t" ;
				if (TURBULENCE_MODEL!=NONE) file << c.k << "\t" << c.omega << "\t" << c.rho*c.k/c.omega ;
				file << endl;
				file.close();
			}
		}

		if ((timeStep) % integrateBoundaryFreq == 0) {
			for (int n=0;n<boundaryFluxes.size();++n) {
				// Sum integrated boundary fluxes accross partitions
				double myFlux[5],integratedFlux[5];
				myFlux[0]=bc.region[boundaryFluxes[n].bc-1].mass;
				for (int i=0;i<3;++i) myFlux[i+1]=bc.region[boundaryFluxes[n].bc-1].momentum[i];
				myFlux[4]=bc.region[boundaryFluxes[n].bc-1].energy;				
				if (np!=1) {
					MPI_Reduce(&myFlux,&integratedFlux,5, MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
					MPI_Barrier(MPI_COMM_WORLD);
				} else {for (int i=0;i<5;++i) integratedFlux[i]=myFlux[i]; }
				// TODO Instead of data transfer between my and integrated 
				// Try MPI_IN_PLACE, also is the Barrier need here?
				if (Rank==0) {
					ofstream file;
					file.open((boundaryFluxes[n].fileName).c_str(),ios::app);
					file << timeStep << setw(16) << setprecision(8)  << "\t" << time;
					for (int i=0;i<5;++i) file << "\t" << integratedFlux[i];
					file << endl;
					file.close();
				}
			}
		}
		firstTimeStep=false;
	
	}

	/*****************************************************************************************/
	// End time loop
	/*****************************************************************************************/
	
	//writeTecplotMacro(restart,timeStepMax, outFreq);
	// Syncronize the processors
	MPI_Barrier(MPI_COMM_WORLD);

	// Report the wall time
	if (Rank==0) {
		timeEnd=MPI_Wtime();
		cout << "* Wall time: " << timeEnd-timeRef << " seconds" << endl;
	}
		
	return 0;
}

double minmod(double a, double b) {
	if ((a*b)<0.) {
		return 0.;
	} else if (fabs(a) < fabs(b)) {
		return a;
	} else {
		return b;
	}
}

double maxmod(double a, double b) {
	if ((a*b)<0.) {
		return 0.;
	} else if (fabs(a) < fabs(b)) {
		return b;
	} else {
		return a;
	}
}

double doubleMinmod(double a, double b) {
	if ((a*b)<0.) {
		return 0.;
	} else {
		return signof(a+b)*min(fabs(2.*a),min(fabs(2.*b),fabs(0.5*(a+b))));
	}
	
}

double harmonic(double a, double b) {
	if ((a*b)<0.) {
		return 0.;
	} else {
		if (fabs(a+b)<1.e-12) return 0.5*(a+b);
		return signof(a+b)*min(fabs(0.5*(a+b)),fabs(2.*a*b/(a+b)));
	}
}

double superbee(double a, double b) {
	return minmod(maxmod(a,b),minmod(2.*a,2.*b));
}


void update(void) {

	resP=0.; resV=0.; resT=0.;
	for (unsigned int c = 0;c < grid.cellCount;++c) {
		grid.cell[c].p +=grid.cell[c].update[0];
		grid.cell[c].v[0] +=grid.cell[c].update[1];
		grid.cell[c].v[1] +=grid.cell[c].update[2];
		grid.cell[c].v[2] +=grid.cell[c].update[3];
		grid.cell[c].T += grid.cell[c].update[4];
		grid.cell[c].rho=eos.rho(grid.cell[c].p,grid.cell[c].T);

		resP+=grid.cell[c].update[0]*grid.cell[c].update[0];
		resV+=grid.cell[c].update[1]*grid.cell[c].update[1]+grid.cell[c].update[2]*grid.cell[c].update[2]+grid.cell[c].update[3]*grid.cell[c].update[3];
		resT+=grid.cell[c].update[4]*grid.cell[c].update[4];
	} // cell loop
	double residuals[3],totalResiduals[3];
	residuals[0]=resP; residuals[1]=resV; residuals[2]=resT;

        if (np!=1) {
        	MPI_Reduce(&residuals,&totalResiduals,3, MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
		resP=totalResiduals[0]; resV=totalResiduals[1]; resT=totalResiduals[2];
        }
	resP=sqrt(resP); resV=sqrt(resV); resT=sqrt(resT);
	return;
}

void update_turb(void) {

	resK=0.; resOmega=0.;
	int counter=0.;
	for (unsigned int c = 0;c < grid.cellCount;++c) {

		// Limit the update_turb so k doesn't end up negative
		grid.cell[c].update_turb[0]=max(-1.*(grid.cell[c].k-kLowLimit),grid.cell[c].update_turb[0]);
		// Limit the update_turb so omega doesn't end up smaller than 100
		grid.cell[c].update_turb[1]=max(-1.*(grid.cell[c].omega-omegaLowLimit),grid.cell[c].update_turb[1]);
		
		double new_mu_t=grid.cell[c].rho*(grid.cell[c].k+grid.cell[c].update_turb[0])/(grid.cell[c].omega+grid.cell[c].update_turb[1]);
		
		if (new_mu_t/viscosity>viscosityRatioLimit) {
			counter++; 
			double under_relax;
			double limit_nu=viscosityRatioLimit*viscosity/grid.cell[c].rho;
			under_relax=(limit_nu*grid.cell[c].omega-grid.cell[c].k)/(grid.cell[c].update_turb[0]-limit_nu*grid.cell[c].update_turb[1]);
			under_relax=max(1.,under_relax);
			grid.cell[c].update_turb[0]*=under_relax;
			grid.cell[c].update_turb[1]*=under_relax;
		}
		
		grid.cell[c].k += grid.cell[c].update_turb[0];
		grid.cell[c].omega+= grid.cell[c].update_turb[1];
		
		resK+=grid.cell[c].update_turb[0]*grid.cell[c].update_turb[0];
		resOmega+=grid.cell[c].update_turb[1]*grid.cell[c].update_turb[1];

	} // cell loop
	if (counter>0) cout << "[I] Update of k and omega is limited due to viscosityRatioLimit constraint for " << counter << " cells" << endl;
	double residuals[2],totalResiduals[2];
	residuals[0]=resK; residuals[1]=resOmega;
	if (np!=1) {
		MPI_Reduce(&residuals,&totalResiduals,2, MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
		resK=totalResiduals[0]; resOmega=totalResiduals[1];
	}
	resK=sqrt(resK); resOmega=sqrt(resOmega);
	return;
}

string int2str(int number) {
	std::stringstream ss;
	ss << number;
	return ss.str();
}

void get_dt(void) {
	
	if (TIME_STEP_TYPE==CFL_MAX) {
		// Determine time step with CFL condition
		dt=1.E20;
		for (cit=grid.cell.begin();cit<grid.cell.end();cit++) {
			double a=sqrt(Gamma*((*cit).p+Pref)/(*cit).rho);
			for (int i=0;i<3;++i) dt=min(dt,CFLmax*(*cit).lengthScale/(fabs((*cit).v[i])+a));
		}
		MPI_Allreduce(&dt,&dt,1, MPI_DOUBLE,MPI_MIN,MPI_COMM_WORLD);
	} 

	return;
} // end get_dt

void get_CFLmax(void) {

	CFLmax=0.;
	for (cit=grid.cell.begin();cit<grid.cell.end();cit++) {
		double a=sqrt(Gamma*((*cit).p+Pref)/(*cit).rho);
		//cout << Gamma <<  "\t" << (*cit).p << "\t" << Pref << "\t" << (*cit).rho << endl;
		for (int i=0;i<3;++i) CFLmax=max(CFLmax,(fabs((*cit).v[0])+a)*dt/(*cit).lengthScale);
	}
	MPI_Allreduce(&CFLmax,&CFLmax,1, MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD);

	return;
}

bool withinBox(Vec3D point,Vec3D corner_1,Vec3D corner_2) {
	for (int i=0;i<3;++i) {
		if (corner_1[i]>corner_2[i]) {
			if (point[i]>corner_1[i]) return false;
			if (point[i]<corner_2[i]) return false;
		} else {
			if (point[i]<corner_1[i]) return false;
			if (point[i]>corner_2[i]) return false;
		}
	}
	return true;
}

bool withinCylinder(Vec3D point,Vec3D center,double radius,Vec3D axisDirection,double height) {
	
	Vec3D onAxis=(point-center).dot(axisDirection)*axisDirection;
	if (fabs(onAxis)>0.5*height) return false;
	
	Vec3D offAxis=(point-center)-onAxis;
	if (fabs(offAxis)>radius) return false;
	
	return true;
}

bool withinSphere(Vec3D point,Vec3D center,double radius) {
	return (fabs(point-center)<=radius);
}
