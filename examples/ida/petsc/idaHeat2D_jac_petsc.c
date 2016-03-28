/*
 * -----------------------------------------------------------------
 * $Revision: 4396 $
 * $Date: 2015-02-26 16:59:39 -0800 (Thu, 26 Feb 2015) $
 * -----------------------------------------------------------------
 * Programmer(s): Slaven Peles @ LLNL
 * (based on PETSc TS example 15 and a SUNDIALS example by 
 *  Allan Taylor, Alan Hindmarsh and Radu Serban)
 * -----------------------------------------------------------------
 * Example problem for IDA: 2D heat equation, using PETSc linear 
 * solver and vector.
 *
 * This example solves a discretized 2D heat equation problem.
 * This version uses the Krylov solver IDASpgmr.
 *
 * The DAE system solved is a spatial discretization of the PDE
 *          du/dt = d^2u/dx^2 + d^2u/dy^2
 * on the unit square. The boundary condition is u = 0 on all edges.
 * Initial conditions are given by u = 16 x (1 - x) y (1 - y).
 * The PDE is treated with central differences on a uniform MX x MY
 * grid. The values of u at the interior points satisfy ODEs, and
 * equations u = 0 at the boundaries are appended, to form a DAE
 * system of size N = MX * MY. Here MX = MY = 10.
 *
 * The system is actually implemented on submeshes, processor by
 * processor, with an MXSUB by MYSUB mesh on each of NPEX * NPEY
 * processors.
 *
 * The system is solved with IDA using default PETSc linear solver
 * The constraints u >= 0 are posed for all components. Local error 
 * testing on the boundary values is suppressed. Output is taken 
 * at t = 0, .01, .02, .04, ..., 10.24.
 * -----------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <ida/ida.h>
#include <ida/ida_petsc.h>
#include <nvector/nvector_petsc.h>
#include <sundials/sundials_types.h>
#include <sundials/sundials_math.h>

#include <mpi.h>
#include <petscdm.h>
#include <petscdmda.h>

#define ZERO  RCONST(0.0)
#define ONE   RCONST(1.0)
#define TWO   RCONST(2.0)

#define NOUT         11             /* Number of output times */

#define NPEX         2              /* No. PEs in x direction of PE array */
#define NPEY         2              /* No. PEs in y direction of PE array */
                                    /* Total no. PEs = NPEX*NPEY */
#define MXSUB        5              /* No. x points per subgrid */
#define MYSUB        5              /* No. y points per subgrid */

#define MX           (NPEX*MXSUB)   /* MX = number of x mesh points */
#define MY           (NPEY*MYSUB)   /* MY = number of y mesh points */
                                    /* Spatial mesh is MX by MY */

typedef struct {  
  DM          da;    /* PETSc data management object */
} *UserData;

/* User-supplied residual function and supporting routines */

int resHeat(realtype tt, N_Vector uu, N_Vector up, N_Vector rr, void *user_data);

int jacHeat(realtype tt,  realtype c_j, 
            N_Vector yy, N_Vector yp, N_Vector resvec,
            Mat Jpre, void *user_data,
            N_Vector tempv1, N_Vector tempv2, N_Vector tempv3);

/* Private function to check function return values */

static int SetInitialProfile(N_Vector uu, N_Vector up, N_Vector id,
                             N_Vector res, void *user_data);

static void PrintHeader(long int Neq, realtype rtol, realtype atol);

static void PrintOutput(int id, void *mem, realtype t, N_Vector uu);

static void PrintFinalStats(void *mem);

static int check_flag(void *flagvalue, char *funcname, int opt, int id);

/*
 *--------------------------------------------------------------------
 * MAIN PROGRAM
 *--------------------------------------------------------------------
 */

int main(int argc, char *argv[])
{
  MPI_Comm comm;
  void *mem;
  UserData data;
  int iout, thispe, ier, npes;
  long int Neq, local_N;
  realtype rtol, atol, t0, t1, tout, tret;
  N_Vector uu, up, constraints, id, res;
  PetscErrorCode ierr;                  /* PETSc error code  */
  Vec uvec;
  Mat Jac;

  mem = NULL;
  data = NULL;
  uu = up = constraints = id = res = NULL;

  /* Get processor number and total number of pe's. */

  MPI_Init(&argc, &argv);
  comm = MPI_COMM_WORLD;
  MPI_Comm_size(comm, &npes);
  MPI_Comm_rank(comm, &thispe);
  
  /* Initialize PETSc */
  ierr = PetscInitializeNoArguments();
  CHKERRQ(ierr);

  if (npes != NPEX*NPEY) {
    if (thispe == 0)
      fprintf(stderr, 
              "\nMPI_ERROR(0): npes = %d is not equal to NPEX*NPEY = %d\n", 
              npes,NPEX*NPEY);
    MPI_Finalize();
    return(1);
  }
  
  /* Set local length local_N and global length Neq. */

  local_N = MXSUB*MYSUB;
  Neq     = MX * MY;
  
  /* Allocate and initialize the data structure and N-vectors. */

  data = (UserData) malloc(sizeof *data);
  if(check_flag((void *)data, "malloc", 2, thispe)) 
    MPI_Abort(comm, 1);
  data->da = NULL;

  ierr = DMDACreate2d(comm, 
                      DM_BOUNDARY_NONE,  /* NONE, PERIODIC, GHOSTED*/
                      DM_BOUNDARY_NONE,
                      DMDA_STENCIL_STAR, /* STAR, BOX */
                      -10,               /* MX */
                      -10,               /* MY */
                      PETSC_DECIDE,      /* NPEX */
                      PETSC_DECIDE,      /* NPEY */
                      1,                 /* degrees of freedom per node */
                      1,                 /* stencil width */
                      NULL,              /* number of nodes per cell in x */
                      NULL,              /* number of nodes per cell in y */
                      &(data->da));
  CHKERRQ(ierr);

  /* PETSc linear solver requires user to create Jacobian matrix */
  ierr = DMSetMatType(data->da, MATAIJ);
  CHKERRQ(ierr);
  ierr  = DMCreateMatrix(data->da, &Jac);
  CHKERRQ(ierr);

  ierr = DMCreateGlobalVector(data->da, &uvec);
  CHKERRQ(ierr);

  /* Make N_Vector wrapper for uvec */
  uu = N_VMake_petsc(&uvec);
  if(check_flag((void *)uu, "N_VNew_petsc", 0, thispe)) 
    MPI_Abort(comm, 1);

  up = N_VClone(uu);
  if(check_flag((void *)up, "N_VNew_petsc", 0, thispe)) 
    MPI_Abort(comm, 1);

  res = N_VClone(uu);
  if(check_flag((void *)res, "N_VNew_petsc", 0, thispe)) 
    MPI_Abort(comm, 1);

  constraints = N_VClone(uu);
  if(check_flag((void *)constraints, "N_VNew_petsc", 0, thispe)) 
    MPI_Abort(comm, 1);

  id = N_VClone(uu);
  if(check_flag((void *)id, "N_VNew_petsc", 0, thispe)) 
    MPI_Abort(comm, 1);

  /* Initialize the uu, up, id, and res profiles. */

  SetInitialProfile(uu, up, id, res, data);
  
  /* Set constraints to all 1's for nonnegative solution values. */

  N_VConst(ONE, constraints);
  
  t0 = ZERO; t1 = RCONST(0.01);
  
  /* Scalar relative and absolute tolerance. */

  rtol = ZERO;
  atol = RCONST(1.0e-3);

  /* Call IDACreate and IDAMalloc to initialize solution. */

  mem = IDACreate();
  if(check_flag((void *)mem, "IDACreate", 0, thispe)) MPI_Abort(comm, 1);

  ier = IDASetUserData(mem, data);
  if(check_flag(&ier, "IDASetUserData", 1, thispe)) MPI_Abort(comm, 1);

  ier = IDASetSuppressAlg(mem, TRUE);
  if(check_flag(&ier, "IDASetSuppressAlg", 1, thispe)) MPI_Abort(comm, 1);

  ier = IDASetId(mem, id);
  if(check_flag(&ier, "IDASetId", 1, thispe)) MPI_Abort(comm, 1);

  ier = IDASetConstraints(mem, constraints);
  if(check_flag(&ier, "IDASetConstraints", 1, thispe)) MPI_Abort(comm, 1);
  N_VDestroy_petsc(constraints);  

  ier = IDAInit(mem, resHeat, t0, uu, up);
  if(check_flag(&ier, "IDAInit", 1, thispe)) MPI_Abort(comm, 1);
  
  ier = IDASStolerances(mem, rtol, atol);
  if(check_flag(&ier, "IDASStolerances", 1, thispe)) MPI_Abort(comm, 1);

  /* Call IDASpgmr to specify the linear solver. */

  ier = IDAPETScKSP(mem, comm, &Jac);
  if(check_flag(&ier, "IDAKSP", 1, thispe)) MPI_Abort(comm, 1);

  ier = IDAPETScSetJacFn(mem, jacHeat);
  if(check_flag(&ier, "IDAPETScSetJacFn", 1, thispe)) MPI_Abort(comm, 1);

  /* Print output heading (on processor 0 only) and intial solution  */
  
  if (thispe == 0) PrintHeader(Neq, rtol, atol);
  PrintOutput(thispe, mem, t0, uu); 
  
  /* Loop over tout, call IDASolve, print output. */

  for (tout = t1, iout = 1; iout <= NOUT; iout++, tout *= TWO) {

    ier = IDASolve(mem, tout, &tret, uu, up, IDA_NORMAL);
    if(check_flag(&ier, "IDASolve", 1, thispe)) MPI_Abort(comm, 1);

    PrintOutput(thispe, mem, tret, uu);

  }
  
  /* Print remaining counters. */

  if (thispe == 0) 
    PrintFinalStats(mem);

  /* Free memory */

  IDAFree(&mem);

  N_VDestroy_petsc(id);
  N_VDestroy_petsc(res);
  N_VDestroy_petsc(up);
  N_VDestroy_petsc(uu);

  ierr = VecDestroy(&uvec);
  CHKERRQ(ierr);
  ierr = MatDestroy(&Jac);
  CHKERRQ(ierr);

  ierr = DMDestroy(&data->da);
  CHKERRQ(ierr);
  free(data);
  
  ierr = PetscFinalize();
  CHKERRQ(ierr);

  MPI_Finalize();

  return(0);

}

/*
 *--------------------------------------------------------------------
 * FUNCTIONS CALLED BY IDA
 *--------------------------------------------------------------------
 */

/*
 * resHeat: heat equation system residual function                       
 * This uses 5-point central differencing on the interior points, and    
 * includes algebraic equations for the boundary values.                 
 * So for each interior point, the residual component has the form       
 *    res_i = u'_i - (central difference)_i                              
 * while for each boundary point, it is res_i = u_i. 
 *                    
 */

int resHeat(realtype tt, 
            N_Vector uu, N_Vector up, N_Vector rr, 
            void *user_data)
{
  PetscErrorCode ierr;
  UserData       data = (UserData) user_data;
  DM             da   = (DM) data->da;
  PetscInt       i,j,Mx,My,xs,ys,xm,ym;
  PetscReal      hx,hy,sx,sy;
  PetscScalar    u,uxx,uyy,**uarray,**f,**udot;
  Vec localU;
  Vec *U    = NV_PVEC_PTC(uu);
  Vec *Udot = NV_PVEC_PTC(up);
  Vec *F    = NV_PVEC_PTC(rr);

  PetscFunctionBeginUser;
  ierr = DMGetLocalVector(da,&localU);CHKERRQ(ierr);
  ierr = DMDAGetInfo(da,PETSC_IGNORE,&Mx,&My,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,
                     PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE);

  hx = 1.0/(PetscReal)(Mx-1); sx = 1.0/(hx*hx);
  hy = 1.0/(PetscReal)(My-1); sy = 1.0/(hy*hy);

  /*
     Scatter ghost points to local vector,using the 2-step process
        DMGlobalToLocalBegin(),DMGlobalToLocalEnd().
     By placing code between these two statements, computations can be
     done while messages are in transition.
  */
  ierr = DMGlobalToLocalBegin(da,*U,INSERT_VALUES,localU);CHKERRQ(ierr);
  ierr = DMGlobalToLocalEnd(da,*U,INSERT_VALUES,localU);CHKERRQ(ierr);

  /* Get pointers to vector data */
  ierr = DMDAVecGetArrayRead(da,localU,&uarray);CHKERRQ(ierr);
  ierr = DMDAVecGetArray(da,*F,&f);CHKERRQ(ierr);
  ierr = DMDAVecGetArray(da,*Udot,&udot);CHKERRQ(ierr);

  /* Get local grid boundaries */
  ierr = DMDAGetCorners(da,&xs,&ys,NULL,&xm,&ym,NULL);CHKERRQ(ierr);

  /* Compute function over the locally owned part of the grid */
  for (j=ys; j<ys+ym; j++) {
    for (i=xs; i<xs+xm; i++) {
      /* Boundary conditions */
      if (i == 0 || j == 0 || i == Mx-1 || j == My-1) {
        f[j][i] = uarray[j][i]; /* F = U */
      } else { /* Interior */
        u = uarray[j][i];
        /* 5-point stencil */
        uxx = (-2.0*u + uarray[j][i-1] + uarray[j][i+1]);
        uyy = (-2.0*u + uarray[j-1][i] + uarray[j+1][i]);
        f[j][i] = udot[j][i] - (uxx*sx + uyy*sy);
      }
    }
  }

  /* Restore vectors */
  ierr = DMDAVecRestoreArrayRead(da,localU,&uarray);CHKERRQ(ierr);
  ierr = DMDAVecRestoreArray(da,*F,&f);CHKERRQ(ierr);
  ierr = DMDAVecRestoreArray(da,*Udot,&udot);CHKERRQ(ierr);
  ierr = DMRestoreLocalVector(da,&localU);CHKERRQ(ierr);
  ierr = PetscLogFlops(11.0*ym*xm);CHKERRQ(ierr);
  
  return (0);
}


/*
 * jacHeat: Heat equation system Jacobian matrix.    
 *                                                                 
 * The optional user-supplied functions jacHeat provides Jacobian 
 * matrix               
 *        J = dF/du + cj*dF/du'                         
 * where the DAE system is F(t,u,u') = 0.         
 *
 */
int jacHeat(realtype tt,  realtype a, 
            N_Vector yy, N_Vector yp, N_Vector resvec,
            Mat Jpre, void *user_data,
            N_Vector tempv1, N_Vector tempv2, N_Vector tempv3)
{
  PetscErrorCode ierr;
  PetscInt       i,j,Mx,My,xs,ys,xm,ym,nc;
  UserData       data = (UserData) user_data;
  DM             da   = (DM) data->da;
  MatStencil     col[5],row;
  PetscScalar    vals[5],hx,hy,sx,sy;

  PetscFunctionBeginUser;
  ierr = DMDAGetInfo(da,PETSC_IGNORE,&Mx,&My,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE);
  ierr = DMDAGetCorners(da,&xs,&ys,NULL,&xm,&ym,NULL);CHKERRQ(ierr);

  hx = 1.0/(PetscReal)(Mx-1); sx = 1.0/(hx*hx);
  hy = 1.0/(PetscReal)(My-1); sy = 1.0/(hy*hy);

  for (j=ys; j<ys+ym; j++) {
    for (i=xs; i<xs+xm; i++) {
      nc    = 0;
      row.j = j; row.i = i;
      if ((i == 0 || i == Mx-1 || j == 0 || j == My-1)) { /* Dirichlet BC */
        col[nc].j = j; col[nc].i = i; vals[nc++] = 1.0; 
      } else {   /* Interior */
        col[nc].j = j-1; col[nc].i = i;   vals[nc++] = -sy;
        col[nc].j = j;   col[nc].i = i-1; vals[nc++] = -sx;
        col[nc].j = j;   col[nc].i = i;   vals[nc++] = 2.0*(sx + sy) + a;
        col[nc].j = j;   col[nc].i = i+1; vals[nc++] = -sx;
        col[nc].j = j+1; col[nc].i = i;   vals[nc++] = -sy;
      }
      ierr = MatSetValuesStencil(Jpre,1,&row,nc,col,vals,INSERT_VALUES);CHKERRQ(ierr);
    }
  }
  ierr = MatAssemblyBegin(Jpre,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(Jpre,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
/*   
     if (J != Jpre) {
       ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
       ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
     }
*/
  PetscFunctionReturn(0);
}



/*
 *--------------------------------------------------------------------
 * PRIVATE FUNCTIONS
 *--------------------------------------------------------------------
 */


/*
 * SetInitialProfile sets the initial values for the problem. 
 */

static int SetInitialProfile(N_Vector uu, N_Vector up, N_Vector id,
                             N_Vector res, void *user_data)
{
  UserData       data = (UserData) user_data;
  DM             da   = data->da;
  PetscErrorCode ierr;
  PetscInt       i,j,xs,ys,xm,ym,Mx,My;
  PetscScalar    **u;
  PetscScalar    **iddat;
  PetscReal      hx,hy,x,y,r;
  Vec *U     = NV_PVEC_PTC(uu);
  Vec *idvec = NV_PVEC_PTC(id);

  PetscFunctionBeginUser;
  ierr = DMDAGetInfo(da,PETSC_IGNORE,&Mx,&My,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,
                     PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE,PETSC_IGNORE);

  hx = 1.0/(PetscReal)(Mx-1);
  hy = 1.0/(PetscReal)(My-1);

  /* Get pointers to vector data */
  ierr = DMDAVecGetArray(da, *U, &u);
  CHKERRQ(ierr);

  /* Get pointers to differentiable variable IDs */
  ierr = DMDAVecGetArray(da, *idvec, &iddat);
  CHKERRQ(ierr);

  /* Get local grid boundaries */
  ierr = DMDAGetCorners(da,&xs,&ys,NULL,&xm,&ym,NULL);CHKERRQ(ierr);

  /* Compute function over the locally owned part of the grid */
  for (j=ys; j<ys+ym; j++) {
    y = j*hy;
    for (i=xs; i<xs+xm; i++) {
      x = i*hx;
      u[j][i] = 16.0 * x*(1.0 - x) * y*(1.0 - y);
      if (i == 0 || j == 0 || i == Mx-1 || j == My-1) {
        iddat[j][i] = 0.0; /* algebraic variables on the boundary */
      } else { 
        iddat[j][i] = 1.0; /* differential variables in the interior */
      }
    }
  }

  /* Restore vectors */
  ierr = DMDAVecRestoreArray(da, *U, &u);
  CHKERRQ(ierr);

   /* Restore vectors */
  ierr = DMDAVecRestoreArray(da, *idvec, &iddat);
  CHKERRQ(ierr);

 /* Initialize up. */
  
  N_VConst(ZERO, up);    /* Initially set up = 0. */
  
  /* resHeat sets res to negative of ODE RHS values at interior points. */
  resHeat(ZERO, uu, up, res, data);
  
  /* Copy -res into up to get correct initial up values. */
  N_VScale(-ONE, res, up);
  

  PetscFunctionReturn(0);
}


/*
 * Print first lines of output and table heading
 */

static void PrintHeader(long int Neq, realtype rtol, realtype atol)
{ 
  printf("\nidaHeat2D_kry_petsc: Heat equation, parallel example problem for IDA\n");
  printf("            Discretized heat equation on 2D unit square.\n");
  printf("            Zero boundary conditions,");
  printf(" polynomial initial conditions.\n");
  printf("            Mesh dimensions: %d x %d", MX, MY);
  printf("        Total system size: %ld\n\n", Neq);
  printf("Subgrid dimensions: %d x %d", MXSUB, MYSUB);
  printf("        Processor array: %d x %d\n", NPEX, NPEY);
#if defined(SUNDIALS_EXTENDED_PRECISION)
  printf("Tolerance parameters:  rtol = %Lg   atol = %Lg\n", rtol, atol);
#elif defined(SUNDIALS_DOUBLE_PRECISION)
  printf("Tolerance parameters:  rtol = %g   atol = %g\n", rtol, atol);
#else
  printf("Tolerance parameters:  rtol = %g   atol = %g\n", rtol, atol);
#endif
  printf("Constraints set to force all solution components >= 0. \n");
  printf("SUPPRESSALG = TRUE to suppress local error testing on ");
  printf("all boundary components. \n");
  printf("Linear solver: IDASPGMR  ");
  printf("Preconditioner: diagonal elements only.\n"); 
  printf("This example uses PETSc vector and linear solver.\n");
  
  /* Print output table heading and initial line of table. */
  printf("\n   Output Summary (umax = max-norm of solution) \n\n");
  printf("  time     umax       k  nst  nni  nli   nre   nreLS    h      nje nps\n");
  printf("----------------------------------------------------------------------\n");
}

/*
 * PrintOutput: print max norm of solution and current solver statistics
 */

static void PrintOutput(int id, void *mem, realtype t, N_Vector uu)
{
  realtype hused, umax;
  long int nst, nni, nje, nre, nreLS=0, nli, npe, nps=0;
  int kused, ier;

  umax = N_VMaxNorm(uu);

  if (id == 0) {

    ier = IDAGetLastOrder(mem, &kused);
    check_flag(&ier, "IDAGetLastOrder", 1, id);
    ier = IDAGetNumSteps(mem, &nst);
    check_flag(&ier, "IDAGetNumSteps", 1, id);
    ier = IDAGetNumNonlinSolvIters(mem, &nni);
    check_flag(&ier, "IDAGetNumNonlinSolvIters", 1, id);
    ier = IDAGetNumResEvals(mem, &nre);
    check_flag(&ier, "IDAGetNumResEvals", 1, id);
    ier = IDAGetLastStep(mem, &hused);
    check_flag(&ier, "IDAGetLastStep", 1, id);
    ier = IDAPETScGetNumJacEvals(mem, &nje);
    check_flag(&ier, "IDAPETScGetNumJtimesEvals", 1, id);
    ier = IDAPETScGetNumLinIters(mem, &nli);
    check_flag(&ier, "IDAPETScGetNumLinIters", 1, id);
//     ier = IDASpilsGetNumResEvals(mem, &nreLS);
//     check_flag(&ier, "IDASpilsGetNumResEvals", 1, id);
//     ier = IDASpilsGetNumPrecEvals(mem, &npe);

#if defined(SUNDIALS_EXTENDED_PRECISION)  
    printf(" %5.2Lf %13.5Le  %d  %3ld  %3ld  %3ld  %4ld        %9.2Le  %3ld    \n",
           t, umax, kused, nst, nni, nli, nre, hused, nje);
#elif defined(SUNDIALS_DOUBLE_PRECISION)  
    printf(" %5.2f %13.5e  %d  %3ld  %3ld  %3ld  %4ld        %9.2e  %3ld    \n",
           t, umax, kused, nst, nni, nli, nre, hused, nje);
#else
    printf(" %5.2f %13.5e  %d  %3ld  %3ld  %3ld  %4ld        %9.2e  %3ld    \n",
           t, umax, kused, nst, nni, nli, nre, hused, nje);
#endif

  }
}

/*
 * Print some final integrator statistics
 */

static void PrintFinalStats(void *mem)
{
  long int netf, ncfn, ncfl;

  IDAGetNumErrTestFails(mem, &netf);
  IDAGetNumNonlinSolvConvFails(mem, &ncfn);
  IDAPETScGetNumConvFails(mem, &ncfl);

  printf("\nError test failures            = %ld\n", netf);
  printf("Nonlinear convergence failures = %ld\n", ncfn);
  printf("Linear convergence failures    = %ld\n", ncfl);
}

/*
 * Check function return value...
 *   opt == 0 means SUNDIALS function allocates memory so check if
 *            returned NULL pointer
 *   opt == 1 means SUNDIALS function returns a flag so check if
 *            flag >= 0
 *   opt == 2 means function allocates memory so check if returned
 *            NULL pointer 
 */

static int check_flag(void *flagvalue, char *funcname, int opt, int id)
{
  int *errflag;

  if (opt == 0 && flagvalue == NULL) {
    /* Check if SUNDIALS function returned NULL pointer - no memory allocated */
    fprintf(stderr, 
            "\nSUNDIALS_ERROR(%d): %s() failed - returned NULL pointer\n\n", 
            id, funcname);
    return(1); 
  } else if (opt == 1) {
    /* Check if flag < 0 */
    errflag = (int *) flagvalue;
    if (*errflag < 0) {
      fprintf(stderr, 
              "\nSUNDIALS_ERROR(%d): %s() failed with flag = %d\n\n", 
              id, funcname, *errflag);
      return(1); 
    }
  } else if (opt == 2 && flagvalue == NULL) {
    /* Check if function returned NULL pointer - no memory allocated */
    fprintf(stderr, 
            "\nMEMORY_ERROR(%d): %s() failed - returned NULL pointer\n\n", 
            id, funcname);
    return(1); 
  }

  return(0);
}