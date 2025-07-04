/*
 * PLUTO: An automatic parallelizer and locality optimizer
 *
 * Copyright (C) 2007-2012 Uday Bondhugula
 *
 * This software is available under the MIT license. Please see LICENSE.MIT
 * in the top-level directory for details.
 *
 * This file is part of libpluto.
 *
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pluto_codegen_if.h"

#include "pluto/internal/ast_transform.h"
#include "constraints.h"
#include "math_support.h"
#include "pluto/matrix.h"
#include "pluto/pluto.h"
#include "program.h"
#include "version.h"

#include "cloog/cloog.h"
#include "osl/extensions/loop.h"

int get_first_point_loop(Stmt *stmt, const PlutoProg *prog) {
  int i, first_point_loop;

  if (stmt->type != ORIG) {
    for (i = 0; i < prog->num_hyperplanes; i++) {
      if (!pluto_is_hyperplane_scalar(stmt, i)) {
        return i;
      }
    }
    /* No non-scalar hyperplanes */
    return 0;
  }

  for (i = stmt->last_tile_dim + 1; i < stmt->trans->nrows; i++) {
    if (stmt->hyp_types[i] == H_LOOP)
      break;
  }

  if (i < prog->num_hyperplanes) {
    first_point_loop = i;
  } else {
    /* Should come here only if
     * it's a 0-d statement */
    first_point_loop = 0;
  }

  return first_point_loop;
}

/* Generate and print .cloog file from the transformations computed */
void pluto_gen_cloog_file(FILE *fp, const PlutoProg *prog) {
  int i;

  Stmt **stmts = prog->stmts;
  int nstmts = prog->nstmts;
  int npar = prog->npar;
  PlutoContext *context = prog->context;

  IF_DEBUG(printf("[pluto] generating Cloog file...\n"));
  fprintf(fp, "# CLooG script generated automatically by PLUTO %s\n",
          PLUTO_VERSION);
  fprintf(fp, "# language: C\n");
  fprintf(fp, "c\n\n");

  /* Context: setting conditions on parameters */
  PlutoConstraints *param_ctx = pluto_constraints_dup(prog->param_context);
  pluto_constraints_intersect_isl(param_ctx, prog->codegen_context);
  pluto_constraints_print_polylib(fp, param_ctx);
  pluto_constraints_free(param_ctx);

  /* Setting parameter names */
  fprintf(fp, "\n1\n");
  for (i = 0; i < npar; i++) {
    fprintf(fp, "%s ", prog->params[i]);
  }
  fprintf(fp, "\n\n");

  fprintf(fp, "# Number of statements\n");
  fprintf(fp, "%d\n\n", nstmts);

  /* Print statement domains */
  for (i = 0; i < nstmts; i++) {
    fprintf(fp, "# S%d (%s)\n", stmts[i]->id + 1, stmts[i]->text);
    pluto_constraints_print_polylib(fp, stmts[i]->domain);
    fprintf(fp, "0 0 0\n\n");
  }

  fprintf(fp, "# we want cloog to set the iterator names\n");
  fprintf(fp, "0\n\n");

  fprintf(fp, "# Number of scattering functions\n");
  if (nstmts >= 1 && stmts[0]->trans != NULL) {
    fprintf(fp, "%d\n\n", nstmts);

    /* Print scattering functions */
    for (i = 0; i < nstmts; i++) {
      fprintf(fp, "# T(S%d)\n", i + 1);
      PlutoConstraints *sched = pluto_stmt_get_schedule(stmts[i]);
      pluto_constraints_print_polylib(fp, sched);
      fprintf(fp, "\n");
      pluto_constraints_free(sched);
    }

    /* Setting target loop names (all stmts have same number of hyperplanes */
    fprintf(fp, "# we will set the scattering dimension names\n");
    fprintf(fp, "%d\n", stmts[0]->trans->nrows);
    for (i = 0; i < stmts[0]->trans->nrows; i++) {
      fprintf(fp, "t%d ", i + 1);
    }
    fprintf(fp, "\n");
  } else {
    fprintf(fp, "0\n\n");
  }
}

static void gen_stmt_macro(const Stmt *stmt, PlutoOptions *options,
                           FILE *outfp) {
  int j;

  for (j = 0; j < stmt->dim; j++) {
    if (stmt->iterators[j] == NULL) {
      printf("Iterator name not set for S%d; required \
                    for generating declarations\n",
             stmt->id + 1);
      assert(0);
    }
  }
  fprintf(outfp, "#define S%d", stmt->id + 1);
  fprintf(outfp, "(");
  for (j = 0; j < stmt->dim; j++) {
    if (j != 0)
      fprintf(outfp, ",");
    fprintf(outfp, "%s", stmt->iterators[j]);
  }
  fprintf(outfp, ")\t");

  /* Generate pragmas for Bee/Cl@k */
  if (options->bee) {
    fprintf(outfp, " __bee_schedule");
    for (j = 0; j < stmt->trans->nrows; j++) {
      fprintf(outfp, "[");
      pluto_affine_function_print(outfp, stmt->trans->val[j], stmt->dim,
                                  (const char **)stmt->iterators);
      fprintf(outfp, "]");
    }
    fprintf(outfp, " _NL_DELIMIT_ ");
  }
  fprintf(outfp, "%s\n", stmt->text);
}

/* Get the string for the induction var type */
static const char *get_indvar_type(int indvar_type) {
  if (indvar_type == 32)
    return "int";
  else if (indvar_type == 64)
    return "long long";
  return NULL;
}

/* Generate variable declarations and macros */
int generate_declarations(const PlutoProg *prog, FILE *outfp) {
  int i;

  Stmt **stmts = prog->stmts;
  int nstmts = prog->nstmts;

  /* Generate statement macros */
  for (i = 0; i < nstmts; i++) {
    gen_stmt_macro(stmts[i], prog->context->options, outfp);
  }
  fprintf(outfp, "\n");

  const char *indvar_type =
      get_indvar_type(prog->context->options->indvar_type);
  if (!indvar_type) {
    fprintf(stderr,
            "Cannot recognize indvar_type: %d, which should be 32 or 64\n",
            prog->context->options->indvar_type);
    exit(1);
  }

  /* Scattering iterators. */
  if (prog->num_hyperplanes >= 1) {
    fprintf(outfp, "\t\t%s ", indvar_type);
    for (i = 0; i < prog->num_hyperplanes; i++) {
      if (i != 0)
        fprintf(outfp, ", ");
      fprintf(outfp, "t%d", i + 1);
      if (prog->hProps[i].unroll) {
        fprintf(outfp, ", t%dt, newlb_t%d, newub_t%d", i + 1, i + 1, i + 1);
      }
    }
    fprintf(outfp, ";\n\n");
  }

  if (prog->context->options->parallel) {
    fprintf(outfp, "\t%s lb, ub, lbp, ubp, lb2, ub2;\n", indvar_type);
  }
  /* For vectorizable loop bound replacement */
  fprintf(outfp, "\tregister %s lbv, ubv;\n\n", indvar_type);

  return 0;
}

/* Call cloog and generate code for the transformed program
 *
 * cloogf, cloogl: set to -1 if you want the function to decide
 *
 * --cloogf, --cloogl overrides everything; next cloogf, cloogl if != -1,
 *  then the function takes care of the rest
 */
int pluto_gen_cloog_code(const PlutoProg *prog, int cloogf, int cloogl,
                         FILE *cloogfp, FILE *outfp) {
  CloogInput *input;
  CloogOptions *cloogOptions;
  CloogState *state;
  PlutoContext *context = prog->context;
  PlutoOptions *options = context->options;
  int i;

  struct clast_stmt *root;

  Stmt **stmts = prog->stmts;
  int nstmts = prog->nstmts;

  state = cloog_state_malloc();
  cloogOptions = cloog_options_malloc(state);

  cloogOptions->fs = (int *)malloc(nstmts * sizeof(int));
  cloogOptions->ls = (int *)malloc(nstmts * sizeof(int));
  cloogOptions->fs_ls_size = nstmts;

  for (i = 0; i < nstmts; i++) {
    cloogOptions->fs[i] = -1;
    cloogOptions->ls[i] = -1;
  }

  cloogOptions->name = (char *)"CLooG file produced by PLUTO";
  cloogOptions->compilable = 0;
  cloogOptions->esp = 1;
  cloogOptions->strides = 1;
  cloogOptions->quiet = !options->debug;

  /* Generates better code in general */
  cloogOptions->backtrack = options->cloogbacktrack;

  if (options->cloogf >= 1 && options->cloogl >= 1) {
    cloogOptions->f = options->cloogf;
    cloogOptions->l = options->cloogl;
  } else {
    if (cloogf >= 1 && cloogl >= 1) {
      cloogOptions->f = cloogf;
      cloogOptions->l = cloogl;
    } else if (options->tile) {
      for (i = 0; i < nstmts; i++) {
        cloogOptions->fs[i] = get_first_point_loop(stmts[i], prog) + 1;
        cloogOptions->ls[i] = prog->num_hyperplanes;
      }
    } else {
      /* Default */
      cloogOptions->f = 1;
      /* last level to optimize: number of hyperplanes;
       * since Pluto provides full-ranked transformations */
      cloogOptions->l = prog->num_hyperplanes;
    }
  }

  if (!options->silent) {
    if (nstmts >= 1 && cloogOptions->fs[0] >= 1) {
      printf("[pluto] using statement-wise -fs/-ls options: ");
      for (i = 0; i < nstmts; i++) {
        printf("S%d(%d,%d), ", i + 1, cloogOptions->fs[i], cloogOptions->ls[i]);
      }
      printf("\n");
    } else {
      printf("[pluto] using Cloog -f/-l options: %d %d\n", cloogOptions->f,
             cloogOptions->l);
    }
  }

  if (options->cloogsh)
    cloogOptions->sh = 1;

  cloogOptions->name = (char *)"PLUTO-produced CLooG file";

  fprintf(outfp, "/* Start of CLooG code */\n");
  /* Get the code from CLooG */
  IF_DEBUG(printf("[pluto] cloog_input_read\n"));
  input = cloog_input_read(cloogfp, cloogOptions);
  IF_DEBUG(printf("[pluto] cloog_clast_create\n"));
  root = cloog_clast_create_from_input(input, cloogOptions);
  if (options->prevector) {
    pluto_mark_vector(root, prog, cloogOptions);
  }
  if (options->parallel) {
    pluto_mark_parallel(root, prog, cloogOptions);
  }
  /* Unroll jamming has to be done at the end. We do not want the epilogue to be
   * marked parallel as there will be very few iterations in it. Properties of
   * the inner loops that are marked PARALLEL or PARALLEL_VEC will be retained
   * during unroll jamming. */
  if (options->unrolljam) {
    pluto_mark_unroll_jam(root, prog, cloogOptions, options->ufactor);
    clast_unroll_jam(root);
  }
  clast_pprint(outfp, root, 0, cloogOptions);
  cloog_clast_free(root);

  fprintf(outfp, "/* End of CLooG code */\n");

  cloog_options_free(cloogOptions);
  cloog_state_free(state);

  return 0;
}

/* Generate code for a single multicore; the ploog script will insert openmp
 * pragmas later */
int pluto_multicore_codegen(FILE *cloogfp, FILE *outfp, const PlutoProg *prog) {
  if (prog->context->options->parallel) {
    fprintf(outfp, "#include <omp.h>\n\n");
  }
  generate_declarations(prog, outfp);

  if (prog->context->options->multipar) {
    fprintf(outfp, "\tomp_set_nested(1);\n");
    fprintf(outfp, "\tomp_set_num_threads(2);\n");
  }

  pluto_gen_cloog_code(prog, -1, -1, cloogfp, outfp);

  return 0;
}

/* Decides which loops to mark parallel and generates the corresponding OpenMP
 * pragmas and writes them out to a file. They are later read by a script
 * (ploog) and appropriately inserted into the output Cloog code
 *
 * Returns: the number of parallel loops for which OpenMP pragmas were generated
 *
 * Generate the #pragma comment -- will be used by a syntactic scanner
 * to put in place -- should implement this with CLast in future */
int pluto_omp_parallelize(PlutoProg *prog) {
  int i;

  FILE *outfp = fopen(".pragmas", "w");

  if (!outfp)
    return 1;

  HyperplaneProperties *hProps = prog->hProps;
  PlutoContext *context = prog->context;
  PlutoOptions *options = context->options;

  int loop;

  /* IMPORTANT: Note that by the time this function is called, pipelined
   * parallelism has already been converted to inner parallelism in
   * tile space (due to a tile schedule) - so we don't need check any
   * PIPE_PARALLEL properties
   */
  /* Detect the outermost sync-free parallel loop - find upto two of them if
   * the multipar option is set */
  int num_parallel_loops = 0;
  for (loop = 0; loop < prog->num_hyperplanes; loop++) {
    if (hProps[loop].dep_prop == PARALLEL && hProps[loop].type != H_SCALAR) {
      // Remember our loops are 1-indexed (t1, t2, ...)
      fprintf(outfp, "t%d #pragma omp parallel for shared(", loop + 1);

      for (i = 0; i < loop; i++) {
        fprintf(outfp, "t%d,", i + 1);
      }

      for (i = 0; i < num_parallel_loops + 1; i++) {
        if (i != 0)
          fprintf(outfp, ",");
        fprintf(outfp, "lb%d,ub%d", i + 1, i + 1);
      }

      fprintf(outfp, ") private(");

      if (options->prevector) {
        fprintf(outfp, "ubv,lbv,");
      }

      /* Lower and upper scalars for parallel loops yet to be marked */
      /* NOTE: we extract up to 2 degrees of parallelism
       */
      if (options->multipar) {
        for (i = num_parallel_loops + 1; i < 2; i++) {
          fprintf(outfp, "lb%d,ub%d,", i + 1, i + 1);
        }
      }

      for (i = loop; i < prog->num_hyperplanes; i++) {
        if (i != loop)
          fprintf(outfp, ",");
        fprintf(outfp, "t%d", i + 1);
      }
      fprintf(outfp, ")\n");

      num_parallel_loops++;

      if (!options->multipar || num_parallel_loops == 2) {
        break;
      }
    }
  }

  IF_DEBUG(fprintf(stdout, "[pluto] marked %d loop(s) parallel\n",
                   num_parallel_loops));

  fclose(outfp);

  return num_parallel_loops;
}

/*
 * Get a list of to-be-parallelized loops frop PlutoProg.
 */
osl_loop_p pluto_get_parallel_loop_list(const PlutoProg *prog,
                                        int vloopsfound) {
  unsigned i, j, nploops;
  osl_loop_p ret_loop = NULL;
  PlutoContext *context = prog->context;

  Ploop **ploops = pluto_get_dom_parallel_loops(prog, &nploops);

  IF_DEBUG(printf("[pluto_parallel_loop_list] parallelizable loops\n"););
  IF_DEBUG(pluto_loops_print(ploops, nploops););

  for (i = 0; i < nploops; i++) {
    osl_loop_p newloop = osl_loop_malloc();

    char iter[13];
    snprintf(iter, sizeof(iter), "t%d", ploops[i]->depth + 1);
    newloop->iter = strdup(iter);

    newloop->nb_stmts = ploops[i]->nstmts;
    newloop->stmt_ids = (int *)malloc(ploops[i]->nstmts * sizeof(int));
    unsigned max_depth = 0;
    for (j = 0; j < ploops[i]->nstmts; j++) {
      Stmt *stmt = ploops[i]->stmts[j];
      newloop->stmt_ids[j] = stmt->id + 1;

      if (stmt->trans->nrows > max_depth)
        max_depth = stmt->trans->nrows;
    }

    newloop->directive += CLAST_PARALLEL_OMP;
    char *private_vars = (char *)malloc(128);
    private_vars[0] = '\0';
    if (vloopsfound)
      strcpy(private_vars, "lbv, ubv");
    unsigned depth = ploops[i]->depth + 1;
    for (depth++; depth <= max_depth; depth++) {
      sprintf(private_vars + strlen(private_vars), "t%d,", depth);
    }
    if (strlen(private_vars))
      private_vars[strlen(private_vars) - 1] = '\0'; // remove last comma
    newloop->private_vars = strdup(private_vars);
    free(private_vars);

    // add new loop to looplist
    osl_loop_add(newloop, &ret_loop);
  }

  pluto_loops_free(ploops, nploops);

  return ret_loop;
}

/// Get a list of to-be-vectorized loops from PlutoProg.
osl_loop_p pluto_get_vector_loop_list(const PlutoProg *prog) {
  unsigned i, j, nploops;
  osl_loop_p ret_loop = NULL;
  PlutoContext *context = prog->context;

  Ploop **ploops = pluto_get_parallel_loops(prog, &nploops);

  for (i = 0; i < nploops; i++) {
    /* Only the innermost ones */
    if (!pluto_loop_is_innermost(ploops[i], prog))
      continue;

    IF_DEBUG(printf("[pluto_get_vector_loop_list] marking loop\n"););
    IF_DEBUG(pluto_loop_print(ploops[i]););

    osl_loop_p newloop = osl_loop_malloc();

    char iter[13];
    snprintf(iter, sizeof(iter), "t%d", ploops[i]->depth + 1);
    newloop->iter = strdup(iter);

    newloop->nb_stmts = ploops[i]->nstmts;
    newloop->stmt_ids = (int *)malloc(ploops[i]->nstmts * sizeof(int));
    for (j = 0; j < ploops[i]->nstmts; j++) {
      newloop->stmt_ids[j] = ploops[i]->stmts[j]->id + 1;
    }

    newloop->directive += CLAST_PARALLEL_VEC;

    // add new loop to looplist
    osl_loop_add(newloop, &ret_loop);
  }

  pluto_loops_free(ploops, nploops);

  return ret_loop;
}

