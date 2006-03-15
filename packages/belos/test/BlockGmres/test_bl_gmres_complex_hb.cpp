// @HEADER
// ***********************************************************************
//
//                 Belos: Block Linear Solvers Package
//                 Copyright (2004) Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
// Questions? Contact Michael A. Heroux (maherou@sandia.gov)
//
// ***********************************************************************
// @HEADER
//
// This driver reads a problem from a Harwell-Boeing (HB) file.
// The right-hand-side from the HB file is used instead of random vectors.
// The initial guesses are all set to zero. 
//
// NOTE: No preconditioner is used in this case. 
//
#include "BelosConfigDefs.hpp"
#include "BelosLinearProblem.hpp"
#include "BelosOutputManager.hpp"
#include "BelosStatusTestMaxIters.hpp"
#include "BelosStatusTestResNorm.hpp"
#include "BelosStatusTestCombo.hpp"
#include "BelosBlockGmres.hpp"
#include "Teuchos_Time.hpp"
#include "Teuchos_CommandLineProcessor.hpp"

// I/O for Harwell-Boeing files
#ifdef HAVE_BELOS_TRIUTILS
#include "iohb.h"
#endif

#include "MyMultiVec.hpp"
#include "MyBetterOperator.hpp"
#include "MyOperator.hpp"

using namespace Teuchos;

int main(int argc, char *argv[]) {
  //
  int info = 0;
  int MyPID = 0;
  bool norm_failure = false;

#ifdef HAVE_MPI	
  // Initialize MPI	
  MPI_Init(&argc,&argv); 	
  MPI_Comm_rank(MPI_COMM_WORLD, &MyPID);
#endif
  //
  using Teuchos::RefCountPtr;
  using Teuchos::rcp;
  Teuchos::Time timer("Belos CG");

  bool verbose = 0;
  std::string filename("mhd1280b.cua");

  CommandLineProcessor cmdp(false,true);
  cmdp.setOption("verbose","quiet",&verbose,"Print messages and results.");
  cmdp.setOption("filename",&filename,"Filename for Harwell-Boeing test matrix.");
  if (cmdp.parse(argc,argv) != CommandLineProcessor::PARSE_SUCCESSFUL) {
#ifdef HAVE_MPI
    MPI_Finalize();
#endif
    return -1;
  }
#ifdef HAVE_COMPLEX
  typedef std::complex<double> ST;
#elif HAVE_COMPLEX_H
  typedef ::complex<double> ST;
#else
  typedef double ST;
  // no complex. quit with failure.
  if (verbose && MyPID == 0) {
    cout << "Not compiled with complex support." << endl;
    cout << "End Result: TEST FAILED" << endl;
#ifdef HAVE_MPI
    MPI_Finalize();
#endif
    return -1;
  }
#endif
  typedef ScalarTraits<ST>                 SCT;
  typedef SCT::magnitudeType                MT;
  typedef Belos::MultiVec<ST>               MV;
  typedef Belos::Operator<ST>               OP;
  typedef Belos::MultiVecTraits<ST,MV>     MVT;
  typedef Belos::OperatorTraits<ST,MV,OP>  OPT;
  ST one  = SCT::one();
  ST zero = SCT::zero();	

  // Create default output manager 
  RefCountPtr<Belos::OutputManager<ST> > MyOM 
    = rcp( new Belos::OutputManager<ST>( MyPID ) );
  // Set verbosity level
  if (verbose) {
    MyOM->SetVerbosity( 2 );
  }

  if (MyOM->doOutput( 2 )) {
    cout << Belos::Belos_Version() << endl << endl;
  }

#ifndef HAVE_BELOS_TRIUTILS
   cout << "This test requires Triutils. Please configure with --enable-triutils." << endl;
#ifdef HAVE_MPI
  MPI_Finalize() ;
#endif
  if (MyPID==0) {
    cout << "End Result: TEST FAILED" << endl;	
  }
  return -1;
#endif

  // Get the data from the HB file
  int dim,dim2,nnz;
  MT *dvals;
  int *colptr,*rowind;
  ST *cvals;
  nnz = -1;
  info = readHB_newmat_double(filename.c_str(),&dim,&dim2,&nnz,
                              &colptr,&rowind,&dvals);
  if (info == 0 || nnz < 0) {
    if (MyPID==0) {
      cout << "Error reading '" << filename << "'" << endl;
      cout << "End Result: TEST FAILED" << endl;
    }
#ifdef HAVE_MPI
    MPI_Finalize();
#endif
    return -1;
  }
  // Convert interleaved doubles to complex values
  cvals = new ST[nnz];
  for (int ii=0; ii<nnz; ii++) {
    cvals[ii] = ST(dvals[ii*2],dvals[ii*2+1]);
  }
  // Build the problem matrix
  RefCountPtr< MyBetterOperator<ST> > A 
    = rcp( new MyBetterOperator<ST>(dim,colptr,nnz,rowind,cvals) );

  //
  // ********Other information used by block solver***********
  // *****************(can be user specified)******************
  //
  int numrhs = 1;  // total number of right-hand sides to solve for
  int blockSize = 1;  // blocksize used by solver
  int maxits = dim/blockSize; // maximum number of iterations to run
  MT tol = 1.0e-7;  // relative residual tolerance
  //
  RefCountPtr<ParameterList> My_PL = rcp( new ParameterList() );
  My_PL->set( "Length", maxits );  // Maximum number of blocks in Krylov factorization
  //
  // Construct the right-hand side and solution multivectors.
  // NOTE:  The right-hand side will be constructed such that the solution is
  // a vectors of one.
  //
  RefCountPtr<MyMultiVec<ST> > soln = rcp( new MyMultiVec<ST>(dim,numrhs) );
  RefCountPtr<MyMultiVec<ST> > rhs = rcp( new MyMultiVec<ST>(dim,numrhs) );
  MVT::MvRandom( *soln );
  OPT::Apply( *A, *soln, *rhs );
  MVT::MvInit( *soln, zero );
  //
  //  Construct an unpreconditioned linear problem instance.
  //
  RefCountPtr<Belos::LinearProblem<ST,MV,OP> > My_LP = 
    rcp( new Belos::LinearProblem<ST,MV,OP>( A, soln, rhs ) );
  My_LP->SetBlockSize( blockSize );
  //
  // *******************************************************************
  // *************Start the block Gmres iteration***********************
  // *******************************************************************
  //
  Belos::StatusTestMaxIters<ST,MV,OP> test1( maxits );
  Belos::StatusTestResNorm<ST,MV,OP> test2( tol );
  RefCountPtr<Belos::StatusTestCombo<ST,MV,OP> > My_Test =
    rcp( new Belos::StatusTestCombo<ST,MV,OP>( Belos::StatusTestCombo<ST,MV,OP>::OR, test1, test2 ) );
  //
  RefCountPtr<Belos::OutputManager<ST> > My_OM = 
    rcp( new Belos::OutputManager<ST>( MyPID ) );
  if (verbose)
    My_OM->SetVerbosity( 2 );	
  //
  Belos::BlockGmres<ST,MV,OP>
    MyBlockGmres( My_LP, My_Test, My_OM, My_PL );
  //
  // **********Print out information about problem*******************
  //
  if (verbose) {
    cout << endl << endl;
    cout << "Dimension of matrix: " << dim << endl;
    cout << "Number of right-hand sides: " << numrhs << endl;
    cout << "Block size used by solver: " << blockSize << endl;
    cout << "Max number of Gmres iterations: " << maxits << endl; 
    cout << "Relative residual tolerance: " << tol << endl;
    cout << endl;
  }
  //
  //
  if (verbose) {
    cout << endl << endl;
    cout << "Running Block Gmres -- please wait" << endl;
    cout << (numrhs+blockSize-1)/blockSize 
	 << " pass(es) through the solver required to solve for " << endl; 
    cout << numrhs << " right-hand side(s) -- using a block size of " << blockSize
	 << endl << endl;
  }
  timer.start(true);
  MyBlockGmres.Solve();
  timer.stop();

  RefCountPtr<MyMultiVec<ST> > temp = rcp( new MyMultiVec<ST>(dim,numrhs) );
  OPT::Apply( *A, *soln, *temp );
  MVT::MvAddMv( one, *rhs, -one, *temp, *temp );
  std::vector<MT> norm_num(numrhs), norm_denom(numrhs);
  MVT::MvNorm( *temp, &norm_num );
  MVT::MvNorm( *rhs, &norm_denom );
  for (int i=0; i<numrhs; ++i) {
    if (verbose) 
      cout << "Relative residual "<<i<<" : " << norm_num[i] / norm_denom[i] << endl;
    if ( norm_num[i] / norm_denom[i] > tol ) {
      norm_failure == true;
      break;
    }
  }
  if (verbose) {
    cout << "Solution time : "<< timer.totalElapsedTime()<<endl;
  }
  
  // Clean up.
  delete [] dvals;
  delete [] colptr;
  delete [] rowind;
  delete [] cvals;

#ifdef HAVE_MPI
    MPI_Finalize();
#endif

  if ( My_Test->GetStatus()!=Belos::Converged || norm_failure ) {
    if (verbose)
      cout << "End Result: TEST FAILED" << endl;	
    return -1;
  }
  //
  // Default return value
  //
  if (verbose)
    cout << "End Result: TEST PASSED" << endl;
  return 0;
  //
} // end test_bl_gmres_complex_hb.cpp
