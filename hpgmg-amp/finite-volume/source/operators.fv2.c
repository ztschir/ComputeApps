/*******************************************************************************
Copyright (c) 2016 Advanced Micro Devices, Inc. 

All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, 
this list of conditions and the following disclaimer in the documentation 
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/
//------------------------------------------------------------------------------------------------------------------------------
// Samuel Williams
// SWWilliams@lbl.gov
// Lawrence Berkeley National Lab
//------------------------------------------------------------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#ifdef USE_GPU
  #if defined(__KALMAR_AMP__) || defined(__HCC_AMP__)
    #include <amp.h>
    #include <amp_math.h>
  #elif defined(__KALMAR_HC__) || defined(__HCC_HC__)
    #include <hc.hpp>
    #include <hc_math.hpp>
  #else
    #error Neither *_AMP__ nor *_HC__ defined; has compiler changed?
  #endif
#endif

//------------------------------------------------------------------------------------------------------------------------------
#ifdef _OPENMP
#include <omp.h>
#endif
//------------------------------------------------------------------------------------------------------------------------------
#include "timers.h"
#include "defines.h"
#include "level.h"
#include "operators.h"
//------------------------------------------------------------------------------------------------------------------------------
#ifndef STENCIL_CONSTANT_COEFFICIENT
#define STENCIL_VARIABLE_COEFFICIENT
#endif
//------------------------------------------------------------------------------------------------------------------------------
#define MyPragma(a) _Pragma(#a)
//------------------------------------------------------------------------------------------------------------------------------
#if (_OPENMP>=201107) // OpenMP 3.1 supports max reductions...
  // XL C/C++ 12.01.0000.0009 sets _OPENMP to 201107, but does not support the max clause within a _Pragma().  
  // This issue was fixed by XL C/C++ 12.01.0000.0011
  // If you do not have this version of XL C/C++ and run into this bug, uncomment these macros...
  //#warning not threading norm() calculations due to issue with XL/C, _Pragma, and reduction(max:bmax)
  //#define PRAGMA_THREAD_ACROSS_BLOCKS(    level,b,nb     )    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1)                     )
  //#define PRAGMA_THREAD_ACROSS_BLOCKS_SUM(level,b,nb,bsum)    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1) reduction(  +:bsum) )
  //#define PRAGMA_THREAD_ACROSS_BLOCKS_MAX(level,b,nb,bmax)    
  #define PRAGMA_THREAD_ACROSS_BLOCKS(    level,b,nb     )    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1)                     )
  #define PRAGMA_THREAD_ACROSS_BLOCKS_SUM(level,b,nb,bsum)    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1) reduction(  +:bsum) )
  #define PRAGMA_THREAD_ACROSS_BLOCKS_MAX(level,b,nb,bmax)    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1) reduction(max:bmax) )
#elif _OPENMP // older OpenMP versions don't support the max reduction clause
  #warning Threading max reductions requires OpenMP 3.1 (July 2011).  Please upgrade your compiler.                                                           
  #define PRAGMA_THREAD_ACROSS_BLOCKS(    level,b,nb     )    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1)                     )
  #define PRAGMA_THREAD_ACROSS_BLOCKS_SUM(level,b,nb,bsum)    MyPragma(omp parallel for private(b) if(nb>1) schedule(static,1) reduction(  +:bsum) )
  #define PRAGMA_THREAD_ACROSS_BLOCKS_MAX(level,b,nb,bmax)    
#else // flat MPI should not define any threading...
  #define PRAGMA_THREAD_ACROSS_BLOCKS(    level,b,nb     )    
  #define PRAGMA_THREAD_ACROSS_BLOCKS_SUM(level,b,nb,bsum)    
  #define PRAGMA_THREAD_ACROSS_BLOCKS_MAX(level,b,nb,bmax)    
#endif


#ifdef USE_GPU
#define GPU_RESTRICT
  #if defined(GPU_TILE_BLOCKS)
    #define BBS GPU_TILE_BLOCKS
  #else
    #define BBS 1
  #endif
  #if defined(GPU_TILE_K)
    #define KBS GPU_TILE_K
  #else
    #define KBS 2
  #endif
  #if defined(GPU_TILE_J)
    #define JBS GPU_TILE_J
  #else
    #define JBS 8
  #endif
  #if defined(GPU_TILE_I)
    #define IBS GPU_TILE_I
  #else
    #define IBS 32
  #endif
#else
#define GPU_RESTRICT __restrict__
#endif
//------------------------------------------------------------------------------------------------------------------------------
#ifdef STENCIL_FUSE_BC
  #error This implementation does not support fusion of the boundary conditions with the operator
#endif
//------------------------------------------------------------------------------------------------------------------------------
void apply_BCs(level_type * level, int x_id, int shape){apply_BCs_v2(level,x_id,shape);}
//------------------------------------------------------------------------------------------------------------------------------
#define Dinv_ijk() Dinv[ijk]        // simply retrieve it rather than recalculating it
//------------------------------------------------------------------------------------------------------------------------------
#if defined(USE_GPU_FOR_SMOOTH)
#ifndef GPU_ARRAY_VIEW

  #define lalpha(index) (alpha[index])

  #ifndef GPU_TILE_DIM
    #define GPU_TILE_DIM 0
  #endif // GPU_TILE_DIM

  #ifndef USE_LDS
  #undef USElxn
  #undef USElbk
  #undef USElbj
  #undef USElbi
  #endif
  #if defined(USE_LDS) &&(BBS>1) && (GPU_DIM==4)
    #error LDS use is not yet implemented for 3D 4 loops with BBS > 1
  #endif
  // All of this LDS initialization is based on STENCIL_SHAPE_STAR.
  // TODO: This could be generalized based on STENCIL_SHAPE and radius,
  // and moved out of the individual operators.*.c

  #ifdef USElxn
    #if (GPU_DIM==4)&&(GPU_TILE_DIM==3)
      #define lxn(inck,incj,inci) (local_x[l_b][l_k+inck+1][l_j+incj+1][l_i+inci+1])
      #define LXN_SIZE (BBS*(KBS+2)*(JBS+2)*(IBS+2))
      #define DECLARE_LXN tile_static double local_x[BBS][KBS+2][JBS+2][IBS+2]
    #else
      #define lxn(inck,incj,inci) (local_x[l_k+inck+1][l_j+incj+1][l_i+inci+1])
      #define LXN_SIZE ((KBS+2)*(JBS+2)*(IBS+2))
      #define DECLARE_LXN tile_static double local_x[KBS+2][JBS+2][IBS+2]
      #define BBS 1
    #endif
    #if (BBS<1)||(KBS<1)||(JBS<1)||(IBS<8)
      #error BBS, KBS, and JBS must be >=1, IBS >= 8 for LDS use.
    #elif (KBS >=2)&&(JBS>=2)
      // In theory  this case can be done with 3 stores.
      // If both KBS and JBS are >= 2 and they are not both 4,
      // it can in theory be done in 2 stores.
      // I'm using 4 because it's easier.
      #define INITIALIZE_LXN(x)                             \
      {                                                     \
        int iadj = (l_i == 0) ? -1 : 1;                     \
        int jadj = (l_j == 0) ? -1 : 1;                     \
        int kadj = (l_k == 0) ? -1 : 1;                     \
        lxn(0,0,iadj) = x[ijk+iadj];                      \
        lxn(0,jadj,0) = x[ijk+jadj*jStride];              \
        lxn(kadj,0,0) = x[ijk+kadj*kStride];              \
        lxn(0,0,0) = x[ijk];                              \
      }
    #elif (KBS==1)&&(JBS==1)
      // Hardest case:
      // We only have IBS threads with which to initialize 5*IBS+2 locations
      // 6 is the minimum number of stores in this case.
      #define INITIALIZE_LXN(x)             \
      {                                     \
        int iadj = (l_i == 0) ? -1 : 1;     \
        lxn( 1,  0,   0) = x[ijk+kStride];  \
        lxn(-1,  0,   0) = x[ijk-kStride];  \
        lxn( 0,  1,   0) = x[ijk+jStride];  \
        lxn( 0, -1,   0) = x[ijk-jStride];  \
        lxn( 0,  0,iadj) = x[ijk+iadj];     \
        lxn( 0,  0,   0) = x[ijk];          \
      }
    #elif (KBS==1)
      // If one of KBS and JBS is 1 and the other 2, this requires a
      // minimum of 5 stores.  If one is 1 and the other >=4, it can
      // be done in 4 stores.
      #define INITIALIZE_LXN(x)                   \
      {                                           \
        int iadj = (l_i == 0) ? -1 : 1;           \
        int jadj = (l_j == 0) ? -1 : 1;           \
        lxn( 1,   0,   0) = x[ijk+kStride];       \
        lxn(-1,   0,   0) = x[ijk-kStride];       \
        lxn( 0,jadj,   0) = x[ijk+jadj*jStride];  \
        lxn( 0,   0,iadj) = x[ijk+iadj];          \
        lxn( 0,   0,   0) = x[ijk];               \
      }
    #elif (JBS==1)
      #define INITIALIZE_LXN(x)                     \
      {                                             \
        int iadj = (l_i == 0) ? -1 : 1;             \
        int kadj = (l_k == 0) ? -1 : 1;             \
        lxn(kadj,  0,   0) = x[ijk+kadj*kStride]; \
        lxn(   0,  1,   0) = x[ijk+jStride];      \
        lxn(   0, -1,   0) = x[ijk-jStride];      \
        lxn(   0,  0,iadj) = x[ijk+iadj];         \
        lxn(   0,  0,   0) = x[ijk];              \
      }
    #else
      #error Error in logic of LDS allocation
    #endif
  #else
    #define lxn(inck,incj,inci) (x_n[ijk+inci+incj*jStride+inck*kStride])
    #define LXN_SIZE (0)
    #define DECLARE_LXN
    #define INITIALIZE_LXN(x)
  #endif

  // Note that, while the use of lxn reaches out +-1 in 3 dimensions,
  // the lb* only reach out +1 in 1 dimension.  This simplifies initialization.
  #ifdef USElbk
    #if (GPU_DIM==4)&&(GPU_TILE_DIM==3)
      #define lbk(inck) (local_beta_k[l_b][l_k+inck][l_j][l_i])
      #define LBK_SIZE (BBS*(KBS+1)*JBS*IBS)
      #define DECLARE_LBK tile_static double local_beta_k[BBS][KBS+1][JBS][IBS]
    #else
      #define lbk(inck) (local_beta_k[l_k+inck][l_j][l_i])
      #define LBK_SIZE ((KBS+1)*JBS*IBS)
      #define DECLARE_LBK tile_static double local_beta_k[KBS+1][JBS][IBS]
    #endif
    #define INITIALIZE_LBK()                              \
    {                                                     \
      lbk(1) = beta_k[ijk+kStride];                       \
      lbk(0) = beta_k[ijk];                               \
    }
  #else
    #define lbk(inck) (beta_k[ijk+inck*kStride])
    #define LBK_SIZE (0)
    #define DECLARE_LBK
    #define INITIALIZE_LBK()
  #endif

  #ifdef USElbj
    #if (GPU_DIM==4)&&(GPU_TILE_DIM==3)
      #define lbj(incj) (local_beta_j[l_b][l_k][l_j+incj][l_i])
      #define LBJ_SIZE (BBS*KBS*(JBS+1)*IBS)
      #define DECLARE_LBJ tile_static double local_beta_j[BBS][KBS][JBS+1][IBS]
    #else
      #define lbj(incj) (local_beta_j[l_k][l_j+incj][l_i])
      #define LBJ_SIZE (KBS*(JBS+1)*IBS)
      #define DECLARE_LBJ tile_static double local_beta_j[KBS][JBS+1][IBS]
    #endif
    #define INITIALIZE_LBJ()                              \
    {                                                     \
      lbj(1) = beta_j[ijk+jStride];                       \
      lbj(0) = beta_j[ijk];                                \
    }
  #else
    #define lbj(incj) (beta_j[ijk+incj*jStride])
    #define LBJ_SIZE (0)
    #define DECLARE_LBJ
    #define INITIALIZE_LBJ()
  #endif

  #ifdef USElbi
    #if (GPU_DIM==4)&&(GPU_TILE_DIM==3)
      #define lbi(inci) (local_beta_i[l_b][l_k][l_j][l_i+inci])
      #define LBI_SIZE (BBS*KBS*JBS*(IBS+1))
      #define DECLARE_LBI tile_static double local_beta_i[BBS][KBS][JBS][IBS+1]
    #else
      #define lbi(inci) (local_beta_i[l_k][l_j][l_i+inci])
      #define LBI_SIZE (KBS*JBS*(IBS+1))
      #define DECLARE_LBI tile_static double local_beta_i[KBS][JBS][IBS+1]
    #endif
    #define INITIALIZE_LBI()                              \
    {                                                     \
      lbi(1) = beta_i[ijk+1];                             \
      lbi(0) = beta_i[ijk];                               \
    }
  #else
    #define lbi(inci) (beta_i[ijk+inci])
    #define LBI_SIZE (0)
    #define DECLARE_LBI
    #define INITIALIZE_LBI()
  #endif
#endif // GPU_ARRAY_VIEW

#ifdef STENCIL_VARIABLE_COEFFICIENT
  #ifdef USE_HELMHOLTZ // variable coefficient Helmholtz...
  #define apply_op_ijk_gpu                    \
  (                                               \
    a*lalpha(ijk)*lxn(0,0,0)                       \
   -b*h2inv*(                                     \
      + lbi(0,0,1)*( lxn( 0, 0, 1) - lxn(0,0,0) ) \
      + lbi(0,0,0)*( lxn( 0, 0,-1) - lxn(0,0,0) ) \
      + lbj(0,1,0)*( lxn( 0, 1, 0) - lxn(0,0,0) ) \
      + lbj(0,0,0)*( lxn( 0,-1, 0) - lxn(0,0,0) ) \
      + lbk(1,0,0)*( lxn( 1, 0, 0) - lxn(0,0,0) ) \
      + lbk(0,0,0)*( lxn(-1, 0, 0) - lxn(0,0,0) ) \
    )                                             \
  )
  #else // variable coefficient Poisson...
  #define apply_op_ijk_gpu                    \
  (                                               \
    -b*h2inv*(                                    \
      + lbi(0,0,1)*( lxn( 0, 0, 1) - lxn(0,0,0) ) \
      + lbi(0,0,0)*( lxn( 0, 0,-1) - lxn(0,0,0) ) \
      + lbj(0,1,0)*( lxn( 0, 1, 0) - lxn(0,0,0) ) \
      + lbj(0,0,0)*( lxn( 0,-1, 0) - lxn(0,0,0) ) \
      + lbk(1,0,0)*( lxn( 1, 0, 0) - lxn(0,0,0) ) \
      + lbk(0,0,0)*( lxn(-1, 0, 0) - lxn(0,0,0) ) \
    )                                             \
  )
  #endif
#else  // constant coefficient case...  
  #define apply_op_ijk_gpu       \
  (                                \
    a*lxn(0,0,0) - b*h2inv*(     \
      + lxn( 0, 0, 1)            \
      + lxn( 0, 0,-1)            \
      + lxn( 0, 1, 0)            \
      + lxn( 0,-1, 0)            \
      + lxn( 1, 0, 0)            \
      + lxn(-1, 0, 0)            \
      - lxn( 0, 0, 0)*6.0        \
    )                              \
  )
#endif // variable/constant coefficient
#define LDS_USE (LXN_SIZE + LBK_SIZE + LBJ_SIZE + LBI_SIZE)
#endif //USE_GPU_FOR_SMOOTH

#if defined(PRINT_DETAILS)
void print_smooth_details(void)
{
  fprintf(stderr, "  Smooth:\n");
  fprintf(stderr, "   fv2 operator\n");

  #ifdef USE_HELMHOLTZ
    fprintf(stderr, "   solving HELMHOLTZ: alpha is used\n");
  #else
    fprintf(stderr, "   solving POISSON: alpha is not used\n");
  #endif // USE_HELMHOLTZ

  #if defined(USE_CHEBY)
    fprintf(stderr, "   CHEBY smoother\n");
  #elif defined(USE_GSRB)
    fprintf(stderr, "   GSRB smoother");
    #if defined(GSRB_FP)
      fprintf(stderr,", FP");
    #elif defined(GSRB_STRIDE2)
      fprintf(stderr,", STRIDE2");
    #elif defined(GSRB_BRANCH)
      fprintf(stderr,", BRANCH");
    #else // GSRB_type
      fprintf(stderr,", UNKNOWN");
    #endif // GSRB_type
    #if defined(GSRB_OOP)
      fprintf(stderr, ", out-of-place\n");
    #else
      fprintf(stderr, ", in-place\n");
    #endif
  #elif defined(USE_JACOBI)
    fprintf(stderr, "   JACOBI smoother\n");
  #elif defined(USE_L1JACOBI)
    fprintf(stderr, "   L1 JACOBI smoother\n");
  #elif defined(USE_SYMGS)
    fprintf(stderr, "   SYMGS smoother\n");
    #if defined(USE_GPU_FOR_SMOOTH)
    #error SYMGS smoother not implemented on GPU
    #endif // USE_GPU_FOR_SMOOTH
  #else
    #error Unknown smoother not implemented on GPU
  #endif
  fprintf(stderr, "    GPU_THRESHOLD is %d\n", GPU_THRESHOLD);

  #ifdef STENCIL_VARIABLE_COEFFICIENT
    fprintf(stderr, "   variable coefficients: ");
    fprintf(stderr, "using beta_k, beta_j, beta_i\n");
  #else // STENCIL_VARIABLE_COEFFICIENT
    fprintf(stderr, "   constant coefficients: ");
    fprintf(stderr, "not using beta_k, beta_j, beta_i\n");
  #endif // STENCIL_VARIABLE_COEFFICIENT

  #ifdef USE_GPU_FOR_SMOOTH
  #if defined(USE_LDS)
  #if defined(GPU_TILE_DIM) && (GPU_TILE_DIM != 0)
  #ifdef USElxn
    fprintf(stderr, "   using LDS for x_n\n");
  #else // USElxn
    fprintf(stderr, "   not using LDS for x_n\n");
  #endif // USElxn

  #ifdef USElbk
          fprintf(stderr, "   using LDS for beta_k\n");
    #if !defined(STENCIL_VARIABLE_COEFFICIENT)
          fprintf(stderr, "   Wasting LDS for beta_k, which is not used in this case\n");
    #endif // STENCIL_VARIABLE_COEFFICIENT
  #else // USElbk
          fprintf(stderr, "   not using LDS for beta_k\n");
  #endif // USElbk

  #ifdef USElbj
    fprintf(stderr, "   using LDS for beta_j\n");
    #if !defined(STENCIL_VARIABLE_COEFFICIENT)
          fprintf(stderr, "   Wasting LDS for beta_j, which is not used in this case\n");
    #endif // STENCIL_VARIABLE_COEFFICIENT
  #else // USElbj
    fprintf(stderr, "   not using LDS for beta_j\n");
  #endif // USElbj

  #ifdef USElbi
    fprintf(stderr, "   using LDS for beta_i\n");
    #if !defined(STENCIL_VARIABLE_COEFFICIENT)
      fprintf(stderr, "   Wasting LDS for beta_i, which is not used in this case\n");
    #endif // STENCIL_VARIABLE_COEFFICIENT
  #else // USElbi
    fprintf(stderr, "   not using LDS for beta_i\n");
  #endif // USElbi


  fprintf(stderr, "   using %d local doubles per large tile\n", LDS_USE);
  if (LDS_USE > 4096) {
    fflush(stdout);
    fflush(stderr);
    fprintf(stderr, "   Too many local doubles\n");
    fflush(stderr);
    exit(0);
  }
  #else // GPU_TILE_DIM
  fprintf(stderr, "   No GPU tiling, so no LDS use\n");
  #endif // GPU_TILE_DIM
  #else // USE_LDS
    fprintf(stderr, "   Not using LDS\n");
  #endif // USE_LDS
  #endif // USE_GPU_FOR_SMOOTH
}

void print_smooth_info(void)
{
  static int printSmoothInfo = 1;
  if (printSmoothInfo) {
    print_smooth_details();
    printSmoothInfo = 0;
  }
}
#endif // PRINT_DETAILS



#ifdef STENCIL_VARIABLE_COEFFICIENT
  #ifdef USE_HELMHOLTZ // variable coefficient Helmholtz...
  #define apply_op_ijk(x)                               \
  (                                                     \
    a*alpha[ijk]*x[ijk]                                 \
   -b*h2inv*(                                           \
      + beta_i[ijk+1      ]*( x[ijk+1      ] - x[ijk] ) \
      + beta_i[ijk        ]*( x[ijk-1      ] - x[ijk] ) \
      + beta_j[ijk+jStride]*( x[ijk+jStride] - x[ijk] ) \
      + beta_j[ijk        ]*( x[ijk-jStride] - x[ijk] ) \
      + beta_k[ijk+kStride]*( x[ijk+kStride] - x[ijk] ) \
      + beta_k[ijk        ]*( x[ijk-kStride] - x[ijk] ) \
    )                                                   \
  )
  #else // variable coefficient Poisson...
  #define apply_op_ijk(x)                               \
  (                                                     \
    -b*h2inv*(                                          \
      + beta_i[ijk+1      ]*( x[ijk+1      ] - x[ijk] ) \
      + beta_i[ijk        ]*( x[ijk-1      ] - x[ijk] ) \
      + beta_j[ijk+jStride]*( x[ijk+jStride] - x[ijk] ) \
      + beta_j[ijk        ]*( x[ijk-jStride] - x[ijk] ) \
      + beta_k[ijk+kStride]*( x[ijk+kStride] - x[ijk] ) \
      + beta_k[ijk        ]*( x[ijk-kStride] - x[ijk] ) \
    )                                                   \
  )
  #endif
#else  // constant coefficient case...  
  #define apply_op_ijk(x)            \
  (                                \
    a*x[ijk] - b*h2inv*(           \
      + x[ijk+1      ]             \
      + x[ijk-1      ]             \
      + x[ijk+jStride]             \
      + x[ijk-jStride]             \
      + x[ijk+kStride]             \
      + x[ijk-kStride]             \
      - x[ijk        ]*6.0         \
    )                              \
  )
#endif // variable/constant coefficient
//------------------------------------------------------------------------------------------------------------------------------
int stencil_get_radius(){return(1);}
int stencil_get_shape(){return(STENCIL_SHAPE_STAR);} // needs just faces
//------------------------------------------------------------------------------------------------------------------------------
void rebuild_operator(level_type * level, level_type *fromLevel, double a, double b){
  // form restriction of alpha[], beta_*[] coefficients from fromLevel
  if(fromLevel != NULL){
    restriction(level,VECTOR_ALPHA ,fromLevel,VECTOR_ALPHA ,RESTRICT_CELL  );
    restriction(level,VECTOR_BETA_I,fromLevel,VECTOR_BETA_I,RESTRICT_FACE_I);
    restriction(level,VECTOR_BETA_J,fromLevel,VECTOR_BETA_J,RESTRICT_FACE_J);
    restriction(level,VECTOR_BETA_K,fromLevel,VECTOR_BETA_K,RESTRICT_FACE_K);
  } // else case assumes alpha/beta have been set

  //no need to extrapolate the beta's into the ghost zones (no mixed derivatives in 2nd order)
  //extrapolate_betas(level);
  //initialize_problem(level,level->h,a,b); // approach used for testing smooth beta's; destroys the black box nature of the solver

  // exchange alpha/beta/...  (must be done before calculating Dinv)
  exchange_boundary(level,VECTOR_ALPHA ,STENCIL_SHAPE_BOX); // safe
  exchange_boundary(level,VECTOR_BETA_I,STENCIL_SHAPE_BOX);
  exchange_boundary(level,VECTOR_BETA_J,STENCIL_SHAPE_BOX);
  exchange_boundary(level,VECTOR_BETA_K,STENCIL_SHAPE_BOX);

  // black box rebuild of D^{-1}, l1^{-1}, dominant eigenvalue, ...
  rebuild_operator_blackbox(level,a,b,2);

  // exchange Dinv/L1inv/...
  exchange_boundary(level,VECTOR_DINV ,STENCIL_SHAPE_BOX); // safe
  exchange_boundary(level,VECTOR_L1INV,STENCIL_SHAPE_BOX);
}


//------------------------------------------------------------------------------------------------------------------------------
#ifdef  USE_GSRB
//#define GSRB_OOP	// no need for out-of-place for 7pt
#define NUM_SMOOTHS      3 // RBRBRB
#include "operators/gsrb.c"
#elif   USE_CHEBY
#define NUM_SMOOTHS      1
#define CHEBYSHEV_DEGREE 6 // i.e. one degree-6 polynomial smoother
#include "operators/chebyshev.c"
#elif   USE_JACOBI
#define NUM_SMOOTHS      6
#include "operators/jacobi.c"
#elif   USE_L1JACOBI
#define NUM_SMOOTHS      6
#include "operators/jacobi.c"
#elif   USE_SYMGS
#define NUM_SMOOTHS      2 // FBFB
#include "operators/symgs.c"
#else
#error You must compile with either -DUSE_GSRB, -DUSE_CHEBY, -DUSE_JACOBI, -DUSE_L1JACOBI, or -DUSE_SYMGS
#endif
#include "operators/residual.c"
#include "operators/apply_op.c"
#include "operators/rebuild.c"
//------------------------------------------------------------------------------------------------------------------------------
#include "operators/blockCopy.c"
#include "operators/misc.c"
#include "operators/exchange_boundary.c"
#include "operators/boundary_fv.c"
#include "operators/restriction.c"
#include "operators/interpolation_v2.c"
//------------------------------------------------------------------------------------------------------------------------------
void interpolation_vcycle(level_type * level_f, int id_f, double prescale_f, level_type *level_c, int id_c){interpolation_v2(level_f,id_f,prescale_f,level_c,id_c);}
void interpolation_fcycle(level_type * level_f, int id_f, double prescale_f, level_type *level_c, int id_c){interpolation_v2(level_f,id_f,prescale_f,level_c,id_c);}
//------------------------------------------------------------------------------------------------------------------------------
#include "operators/problem.fv.c"
//------------------------------------------------------------------------------------------------------------------------------
