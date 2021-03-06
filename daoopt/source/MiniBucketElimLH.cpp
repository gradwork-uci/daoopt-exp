/*
 * MiniBucket.cpp
 *
 *  Copyright (C) 2008-2012 Lars Otten
 *  This file is part of DAOOPT.
 *
 *  DAOOPT is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  DAOOPT is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with DAOOPT.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Created on: Nov 8, 2008
 *      Author: Lars Otten <lotten@ics.uci.edu>
 */

#include <cstddef>
#include <fstream>
#include <chrono>
#include <gflags/gflags.h>

DECLARE_bool(bee_importance_sampling);
DECLARE_double(lookahead_starting_probability);
DECLARE_double(lookahead_min_probability);
DECLARE_bool(lookahead_fix_probability);
DECLARE_bool(lookahead_always_perform_if_better_once);
DECLARE_bool(lookahead_always_compute);

DECLARE_bool(aobf_subordering_use_relative_error);
DECLARE_int32(aobf_subordering_depth_limit);

using namespace std::chrono;

#include "MiniBucketElimLH.h"

/* disables DEBUG output */
#undef DEBUG

static int nFNsBEexact = 0, nFNsBEsampled = 0;

/*
   extern int64 nd1SpecialCalls;
   extern int64 nd1GeneralCalls;
   */

//#define DEBUG_BUCKET_ERROR
//#define NO_LH_PREPROCESSING

#ifndef OUR_OWN_nInfinity
#define OUR_OWN_nInfinity (-std::numeric_limits<double>::infinity())
#endif  // OUR_OWN_nInfinity
#ifndef OUR_OWN_pInfinity
#define OUR_OWN_pInfinity (std::numeric_limits<double>::infinity())
#endif  // OUR_OWN_pInfinity

//#define USE_H_RESIDUAL

namespace daoopt {

high_resolution_clock::time_point _lh_time_start, _lh_time_stop;

static bool sort_pair_vec(std::pair<int, int> &el1, std::pair<int, int> &el2) {
  return el1.first < el2.first;
}

static int computeMBOutFnArgsVectorPtrMap(int elim_var,
                                          vector<Function *> &Functions,
                                          vector<int> &scope, int &n,
                                          val_t *&tuple,
                                          vector<vector<val_t *>> &idxMap) {
  idxMap.clear();
  idxMap.resize(Functions.size());

  int i;
  uint64 j;

  vector<Function *>::const_iterator fit;
  vector<int>::const_iterator sit;

  set<int> scope_;
  for (fit = Functions.begin(); fit != Functions.end(); ++fit)
    scope_.insert((*fit)->getScopeVec().begin(), (*fit)->getScopeVec().end());
  scope_.erase(elim_var);

  scope.clear();
  scope.insert(scope.end(), scope_.begin(), scope_.end());

  if (NULL == tuple) {
    n = scope.size();          // new function arity
    tuple = new val_t[n + 1];  // this keeps track of the tuple assignment
  }
  val_t *elimVal = tuple + n;  // n+1 is elimVar

  // holds iterators .begin() and .end() for all function scopes
  vector<pair<set<int>::const_iterator, set<int>::const_iterator>> iterators;
  iterators.reserve(Functions.size());
  for (fit = Functions.begin(); fit != Functions.end(); ++fit) {
    // store begin() and end() for each function scope
    iterators.push_back(
        make_pair((*fit)->getScopeSet().begin(), (*fit)->getScopeSet().end()));
  }

  // collect pointers to tuple values
  bool bucketVarPassed = false;
  for (i = 0, sit = scope.begin(); i < n; ++i, ++sit) {
    if (!bucketVarPassed && *sit > elim_var) {  // just went past bucketVar
      for (j = 0; j < Functions.size(); ++j) {
        idxMap[j].push_back(elimVal);
        ++(iterators[j]
               .first);  // skip bucketVar in the original function scope
      }
      bucketVarPassed = true;
    }

    for (j = 0; j < Functions.size(); ++j) {
      if (iterators[j].first != iterators[j].second  // scope iterator != end()
          && *sit == *(iterators[j].first)) {        // value found
        idxMap[j].push_back(&tuple[i]);
        ++(iterators[j].first);
      }
    }
  }

  if (!bucketVarPassed && elim_var < INT_MAX) {  // bucketVar has highest index
    for (j = 0; j < Functions.size(); ++j) idxMap[j].push_back(elimVal);
  }

  return 0;
}

void MiniBucketElimLH::reset(void) {
  //	for (MiniBucketElimLHError & be_error : _LookaheadResiduals)
  //		be_error.Delete();
  for (MBLHSubtree &be_lh : _Lookahead) be_lh.Delete();
  deleteLocalErrorFNs();

  for (set<int> &scope : _BucketScopes) scope.clear();
  for (vector<Function *> &b_fns : _BucketFunctions) b_fns.clear();
  _BucketScopes.clear();
  _BucketFunctions.clear();

  _Stats.reset();
  _Stats._NumNodesLookahead.resize(m_problem->getN(), 0);
  _Stats._NumNodesLookaheadSkipped.resize(m_problem->getN(), 0);

  MiniBucketElim::reset();
}

int MiniBucketElimLH::Compute_distToClosestDescendantWithLE(void) {
  vector<int> btOrder;
  findDfsOrder(btOrder);

  // for each bucket, compute closest distance to descendant with >0 bucket
  // error.
  for (vector<int>::reverse_iterator itV = btOrder.rbegin();
       itV != btOrder.rend(); ++itV) {
    int v = *itV;  // this is the variable being eliminated
    PseudotreeNode *parent_n = m_pseudotree->getNode(v)->getParent();
    if (NULL == parent_n) continue;
    int parent_v = parent_n->getVar(), d2parent_v = INT_MAX;
    if (_BucketErrorQuality[v] > 1)
      d2parent_v = 0;  // this bucket has substantial error
    else
      d2parent_v = _distToClosestDescendantWithLE[v];
    if (d2parent_v >= INT_MAX) continue;
    if (++d2parent_v < _distToClosestDescendantWithLE[parent_v])
      _distToClosestDescendantWithLE[parent_v] = d2parent_v;
    // do consistency check : _distToClosestDescendantWithMBs[i] <=
    // _distToClosestDescendantWithLE[i]
    if (_distToClosestDescendantWithMBs[v] >
        _distToClosestDescendantWithLE[v]) {
      int bug_here = 1;
    }
  }
  return 0;
}

/*void MiniBucketElimLH::Delete(void)
  {
  for (auto & aug_fns : m_augmented) {
  for (Function *fn : aug_fns) {
  delete fn ;
  }
  }
  m_augmented.clear() ;
  m_intermediate.clear() ;
  }
  */
size_t MiniBucketElimLH::build(const std::vector<val_t> *assignment,
                               bool computeTables) {

#if defined DEBUG || _DEBUG
  cout << "$ Building MBEX(" << m_ibound << ")" << endl;
#endif

  reset();
  if (computeTables) {
    LPReparameterization();
  }

  _Stats._iBound = m_ibound;

  vector<int> elimOrder;    // will hold dfs order
  findDfsOrder(elimOrder);  // computes dfs ordering of relevant subtree

  // to define m_augmented/m_intermediate, let v be a variable being eliminated,
  // j be the mini-bucket of v being processed,
  // and u be the closest (to the bucket of v) bucket that contains a variable
  // in fn_v_j, where fn_v_j is the fn produced by minibucket j of v.
  // m_augmented is a list of functions generated by the minibucket algorithm
  // that are placed in the bucket of u.
  // m_intermediate is a list of functions generated by the minibucket algorithm
  // that are placed in bucket between (u,v).
  m_augmented.resize(m_problem->getN());
  m_intermediate.resize(m_problem->getN());

  // compute depth of each bucket
  _MaxDepth = -1;
  _Depth.resize(m_problem->getN());
  for (int i = m_problem->getN() - 1; i >= 0; i--) _Depth[i] = -1;
  for (vector<int>::iterator itV = elimOrder.begin(); itV != elimOrder.end();
       ++itV) {
    int v = *itV;  // this is the variable being eliminated
    PseudotreeNode *n = m_pseudotree->getNode(v);
    PseudotreeNode *p = n->getParent();
    if (NULL == p)
      _Depth[v] = 0;
    else {
      int u = p->getVar();
      _Depth[v] = 1 + _Depth[u];
    }
    if (_Depth[v] > _MaxDepth) _MaxDepth = _Depth[v];
  }

  // keep track of total memory consumption
  _Stats._MemorySize = 0;

  _BucketScopes.resize(m_problem->getN());
  _BucketFunctions.resize(m_problem->getN());
  //	_BucketHFnScopes.resize(m_problem->getN()) ;
  _MiniBuckets.resize(m_problem->getN());
  //	_LookaheadResiduals.resize(m_problem->getN());
  _Lookahead.resize(m_problem->getN());
  _BucketErrorQuality.resize(m_problem->getN(), -1);
  _BucketError_AbsAvg.resize(m_problem->getN(), OUR_OWN_nInfinity);
  _BucketError_AbsMin.resize(m_problem->getN(), OUR_OWN_nInfinity);
  _BucketError_AbsMax.resize(m_problem->getN(), OUR_OWN_nInfinity);
  _BucketError_Rel.resize(m_problem->getN(), 0);
  _Pseudowidth.resize(m_problem->getN(), -1);
  _distToClosestDescendantWithMBs.resize(m_problem->getN(), INT_MAX);
  _nLHcalls.resize(m_problem->getN(), 0);
  _nLHcallsSkipped.resize(m_problem->getN(), 0);
  _distToClosestDescendantWithLE.resize(m_problem->getN(), INT_MAX);

  lookahead_probability_.resize(m_problem->getN(),
                                FLAGS_lookahead_starting_probability);
  count_better_ordering_.resize(m_problem->getN(), 0);
  deleteLocalErrorFNs();
  for (int i = m_problem->getN() - 1; i >= 0; i--) {
    // _LookaheadResiduals[i].Delete() ;
    _Lookahead[i].Delete();
  }

  // ITERATES OVER BUCKETS, FROM LEAVES TO ROOT
  for (vector<int>::reverse_iterator itV = elimOrder.rbegin();
       itV != elimOrder.rend(); ++itV) {
    int v = *itV;  // this is the variable being eliminated
    // partition functions into minibuckets
    vector<MiniBucket> &minibuckets = _MiniBuckets[v];
    minibuckets.clear();

    vector<Function *> &funs =
        _BucketFunctions[v];  // this is the list of all (original +
                              // MB-generated) in the bucket of v
    funs.clear();
    set<int> &jointScope = _BucketScopes[v];
    jointScope.clear();

#if defined DEBUG || _DEBUG
    cout << "$ Bucket for variable " << *itV << endl;
    if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
      fprintf(m_options->_fpLogFile, "\nBucket for variable %d(domainsize=%d)",
              (int)*itV, (int)m_problem->getDomainSize(v));
    }
#endif

    // add original functions
    const vector<Function *> &fnlist = m_pseudotree->getFunctions(v);
    funs.insert(funs.end(), fnlist.begin(), fnlist.end());
    // compute width
    for (vector<Function *>::iterator itF = funs.begin(); itF != funs.end();
         ++itF)
      jointScope.insert((*itF)->getScopeVec().begin(),
                        (*itF)->getScopeVec().end());
    double stats_Width = jointScope.size();
    if (_Stats._Width < int(jointScope.size()))
      _Stats._Width = jointScope.size();
    // add MB-generated functions
    funs.insert(funs.end(), m_augmented[v].begin(), m_augmented[v].end());
    // compute pseudo-width
    for (vector<Function *>::iterator itF = m_augmented[v].begin();
         itF != m_augmented[v].end(); ++itF)
      jointScope.insert((*itF)->getScopeVec().begin(),
                        (*itF)->getScopeVec().end());
    double stats_PseudoWidth = jointScope.size();
    _Pseudowidth[v] = jointScope.size();
    if (_Stats._PseudoWidth < int(jointScope.size()))
      _Stats._PseudoWidth = jointScope.size();
#if defined DEBUG || _DEBUG
    for (vector<Function *>::iterator itF = funs.begin(); itF != funs.end();
         ++itF)
      cout << ' ' << (**itF);
    cout << endl;
#endif
#if defined DEBUG || _DEBUG
    size_t tablesize = 1;
    if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
      for (const int scope_var : jointScope) {
        if (v == scope_var) {
          continue;
        }
        tablesize *= m_problem->getDomainSize(scope_var);
      }
      fprintf(m_options->_fpLogFile,
              " Width=%d PseudoWidth=%d OUTtablesize=%lld", (int)stats_Width,
              (int)stats_PseudoWidth, (int64)tablesize);
    }
#endif

    // compute global upper bound for root (dummy) bucket
    if (v == elimOrder[0]) {  // variable is dummy root variable
      if (computeTables &&
          assignment) {  // compute upper bound if assignment is given
        m_globalUB = ELEM_ONE;
        for (vector<Function *>::iterator itF = funs.begin(); itF != funs.end();
             ++itF)
          m_globalUB OP_TIMESEQ(*itF)->getValue(*assignment);
        cout << "    MBE-ALL  = " << SCALE_LOG(m_globalUB) << " ("
             << SCALE_NORM(m_globalUB) << ")" << endl;
        m_globalUB OP_DIVIDEEQ m_problem->globalConstInfo();  // for backwards
                                                              // compatibility
                                                              // of output
        cout << "    MBE-ROOT = " << SCALE_LOG(m_globalUB) << " ("
             << SCALE_NORM(m_globalUB) << ")" << endl;
      }
      continue;  // skip the dummy variable's bucket
    }

    // sort functions by decreasing scope size
    sort(funs.begin(), funs.end(), scopeIsLarger);

    for (vector<Function *>::iterator itF = funs.begin(); itF != funs.end();
         ++itF) {
      bool placed = false;
      for (vector<MiniBucket>::iterator itB = minibuckets.begin();
           !placed && itB != minibuckets.end(); ++itB) {
        if (itB->allowsFunction(*itF)) {  // checks if function fits into bucket
          itB->addFunction(*itF);
          placed = true;
        }
      }
      if (!placed) {  // no fit, need to create new bucket
        MiniBucket mb(v, m_ibound, m_problem);
        mb.addFunction(*itF);
        minibuckets.push_back(mb);
      }
    }

    // Moment-matching step, performed only if we have partitioning.
    vector<Function *> max_marginals;
    std::unique_ptr<Function> average_mm_function;
    if (computeTables && m_options->match && minibuckets.size() > 1) {
      set<int> scope_intersection;
      bool first_mini_bucket = true;
      for (const MiniBucket &mini_bucket : minibuckets) {
        if (first_mini_bucket) {
          scope_intersection = mini_bucket.getJointScope();
          first_mini_bucket = false;
        } else
          scope_intersection =
              intersection(scope_intersection, mini_bucket.getJointScope());
      }
      for (MiniBucket &mini_bucket : minibuckets) {
        set<int> elim_vars =
            setminus(mini_bucket.getJointScope(), scope_intersection);
        max_marginals.push_back(
            mini_bucket.eliminate(computeTables, elim_vars));
      }

      // Find average max-marginals (geometric mean)
      size_t table_size = 1;
      for (const int var : scope_intersection)
        table_size *= m_problem->getDomainSize(var);

      double *average_mm_table = new double[table_size];
      for (size_t i = 0; i < table_size; ++i) average_mm_table[i] = ELEM_ONE;
      for (const Function *max_marginal : max_marginals) {
        for (size_t i = 0; i < table_size; ++i)
          average_mm_table[i] OP_TIMESEQ max_marginal->getTable()[i];
      }
      for (size_t i = 0; i < table_size; ++i)
        average_mm_table[i] = OP_ROOT(average_mm_table[i], minibuckets.size());
      int dummy_id = 0;
      average_mm_function.reset(
          new FunctionBayes(dummy_id, m_problem, scope_intersection,
                            average_mm_table, table_size));
    }

    int nMBs = minibuckets.size();
    if (_Stats._MaxNumMBs < nMBs) _Stats._MaxNumMBs = nMBs;
    if (nMBs > 1) (_Stats._NumBucketsWithMoreThan1MB)++;

#if defined DEBUG || _DEBUG
    if (NULL != m_options ? NULL != m_options->_fpLogFile : false)
      fprintf(m_options->_fpLogFile, " nMBs=%d", (int)minibuckets.size());
#endif

    // minibuckets for current bucket are now ready, process each and place
    // resulting function
    uint32 bucket_idx = 0;
    for (vector<MiniBucket>::iterator itB = minibuckets.begin();
         itB != minibuckets.end(); ++itB, ++bucket_idx) {
      Function *newf = NULL;
      if (!computeTables || !m_options->match || minibuckets.size() <= 1) {
        newf = itB->eliminate(computeTables);  // process the minibucket
      } else {
        newf = itB->eliminateMM(computeTables, max_marginals[bucket_idx],
                                average_mm_function.get());
      }
      const set<int> &newscope = newf->getScopeSet();
      _Stats._MemorySize += newf->getTableSize();
      // go up in tree to find target bucket
      PseudotreeNode *n = m_pseudotree->getNode(v)->getParent();
      while (newscope.find(n->getVar()) == newscope.end() &&
             n != m_pseudotree->getRoot()) {
        m_intermediate[n->getVar()].push_back(newf);
        n = n->getParent();
      }
      // matching bucket found OR root of pseudo tree reached
      m_augmented[n->getVar()].push_back(newf);
    }
    // all minibuckets processed and resulting functions placed
  }

#if defined DEBUG || _DEBUG
  // output augmented and intermediate buckets
  if (computeTables) {
    for (int i = 0; i < m_problem->getN(); i++) {
      cout << "$ AUG" << i << ": " << m_augmented[i] << " + "
           << m_intermediate[i] << endl;
    }
  }
#endif

  // for each bucket, compute closest distance to descendant with more than 1
  // MB.
  for (vector<int>::reverse_iterator itV = elimOrder.rbegin();
       itV != elimOrder.rend(); ++itV) {
    int v = *itV;  // this is the variable being eliminated
    vector<MiniBucket> &minibuckets = _MiniBuckets[v];
    PseudotreeNode *parent_n = m_pseudotree->getNode(v)->getParent();
    if (NULL == parent_n) continue;
    int parent_v = parent_n->getVar(), d2parent_v = INT_MAX;
    if (minibuckets.size() > 1)
      d2parent_v = 0;
    else
      d2parent_v = _distToClosestDescendantWithMBs[v];
    if (d2parent_v >= INT_MAX) continue;
    if (++d2parent_v < _distToClosestDescendantWithMBs[parent_v])
      _distToClosestDescendantWithMBs[parent_v] = d2parent_v;
  }

  // some statistic about local error and lookahead.
  int LH_nNodesWithDescendants = 0, LH_nTotalDescendants = 0;
  int LH_minDepthOfNodeWithLookahead = INT_MAX,
      LH_maxDepthOfNodeWithLookahead = -1;

  size_t mem_size = _Stats._MemorySize;
  double minibucket_mem_mb = mem_size * sizeof(double) / (1024.0 * 1024);

  // clean up for estimation mode
  if (!computeTables) {
    this->reset();
  } else {
    double total_memory_limit = m_options->lookahead_LE_AllTablesTotalLimit;
    double table_memory_limit = m_options->lookahead_LE_SingleTableLimit;

    // only compute local errors as needed for subtree pruning or
    // aobf subproblem ordering heuristic
    if (!m_options->lookahead_use_full_subtree ||
        m_options->aobf_subordering != "") {
      computeLocalErrorTables(true, total_memory_limit, table_memory_limit);
    } else {
      for (int v : elimOrder) {
        _BucketErrorQuality[v] = 99;
      }
    }

    // for each bucket, compute closest distance to descendant with >0 bucket
    // error
    if (m_options->lookaheadDepth > 0 ||
        m_options->lookaheadSubtreeSizeLimit > 0) {
      // compute subtrees for lookahead computation
      int shallowDepthLimit = _MaxDepth / 5;  // 20% of shallowest
      if (shallowDepthLimit > 50) shallowDepthLimit = 50;

      SetupLookaheadStructure_FractionOfLargestAbsErrorNodesOnly(
          *this, m_options->lookaheadDepth,
          m_options->lookaheadSubtreeSizeLimit);
      int nWithLE = 0;
      for (auto itV = elimOrder.begin(); itV != elimOrder.end(); ++itV) {
        int v = *itV;
        if (_BucketErrorQuality[v] > 1) nWithLE++;
      }
      if (m_options->_fpLogFile) {
        fprintf(m_options->_fpLogFile, "\n   nBucketsWithLE>1 = %d", nWithLE);
        fflush(m_options->_fpLogFile);
      }

      // compute statistic for lookahead subtrees
      for (auto itV = elimOrder.begin(); itV != elimOrder.end(); ++itV) {
        int v = *itV;
        if (_Lookahead[v]._SubtreeNodes.size() > 0) {
          int depth = _Depth[v];
          ++LH_nNodesWithDescendants;
          LH_nTotalDescendants += (int)_Lookahead[v]._SubtreeNodes.size();
          if (LH_minDepthOfNodeWithLookahead > depth)
            LH_minDepthOfNodeWithLookahead = depth;
          if (LH_maxDepthOfNodeWithLookahead < depth)
            LH_maxDepthOfNodeWithLookahead = depth;
        }
      }
    }
  }

  // We want to capture the amount of "work per variable".
  // No lookahead does 1 unit of work, and lookahead does 1+the number of
  // descendants in that lookahead tree.
  double LH_averageLookaheadTreeSize =
      double(elimOrder.size() + LH_nTotalDescendants) / elimOrder.size();

  if (computeTables) {
    cout << "Pseudowidth: " << _Stats._PseudoWidth - 1 << endl;
    cout << "Minibucket Memory (MB): " << minibucket_mem_mb << endl;
    cout << "Local Error Memory (MB): " << _Stats._LEMemorySizeMB << endl;
    cout << "Total Heuristic Memory (MB): "
         << minibucket_mem_mb + _Stats._LEMemorySizeMB << endl;
    cout << "LH nBucketsWithNonZeroBucketError: "
         << _nBucketsWithNonZeroBucketError
         << " nBucketsWithMoreThan1MB: " << _nBucketsWithMoreThan1MB << endl;
    cout << "LH nNodesWithDescendants: " << LH_nNodesWithDescendants
         << " nTotalDescendants: " << LH_nTotalDescendants
         << " (BucketErrorIgnoreThreshold="
         << m_options->lookahead_LE_IgnoreThreshold << ")" << endl;
    cout << "LH averageLookaheadTreeSize: " << LH_averageLookaheadTreeSize
         << endl;
    cout << "LH minDepthOfNodeWithLookahead: " << LH_minDepthOfNodeWithLookahead
         << " maxDepthOfNodeWithLookahead: " << LH_maxDepthOfNodeWithLookahead
         << " (MaxDepth=" << _MaxDepth << ")" << endl;
  }
  if (m_options->lookaheadDepth > 0 ||
      m_options->lookaheadSubtreeSizeLimit > 0) {
    int64 nLHSubtreeNodesEvaluated = 0;
    int64 nTotalSubtreeNodes = 0;
    int64 nSubtreeNodesIndependentOfContext = 0;
    int nNodesWithLH = 0;
    int nCopySubtrees = 0;
    for (daoopt::MBLHSubtree &st : _Lookahead) {
      if (0 == st._SubtreeNodes.size()) continue;  // empty subtree
      ++nNodesWithLH;
      if (NULL != st._IsCopyOfEarlierSubtree) nCopySubtrees++;
      nLHSubtreeNodesEvaluated += st.nSubtreeNodesEvaluated();
      nTotalSubtreeNodes += st._SubtreeNodes.size();
      nSubtreeNodesIndependentOfContext +=
          st._nSubtreeNodesIndependentOfContext;
    }
    if (m_options->_fpLogFile && computeTables) {
      fprintf(m_options->_fpLogFile,
              "\nnNodesWithLH=%d nCopies=%d nTotalSubtreeNodes=%lld "
              "nSubtreeNodesIndependentOfContext=%lld "
              "nLHSubtreeNodesEvaluated=%lld",
              nNodesWithLH, nCopySubtrees, nTotalSubtreeNodes,
              nSubtreeNodesIndependentOfContext, nLHSubtreeNodesEvaluated);
      fflush(m_options->_fpLogFile);
    }
  }

  return mem_size;
}

double MiniBucketElimLH::getHeurPerIndSubproblem(
    int var, std::vector<val_t> &assignment, SearchNode *search_node,
    double label, std::vector<double> &subprobH) {
  exit(991);  // this code is no longer good since _LookaheadResiduals[] is
              // obsolete (though still usable).

  double h = MiniBucketElim::getHeurPerIndSubproblem(
      var, assignment, search_node, label, subprobH);

  if (m_options->lookaheadDepth <= 0) return h;
  if (_distToClosestDescendantWithLE[var] > m_options->lookaheadDepth) return h;

  double current_gap = OUR_OWN_pInfinity;
  // the parent should be an AND node; get its pruning gap; it should be >0
  // since pruning check has failed if we got here.
  // we will check if the current h is enough to cover the pruning gap.
  SearchNodeOR *this_node = dynamic_cast<SearchNodeOR *>(search_node);
  SearchNodeAND *parent_node =
      dynamic_cast<SearchNodeAND *>(search_node->getParent());
  double pg = parent_node->PruningGap();  // should be > 0, otherwise the parent
                                          // would have been pruned
  if (!std::isnan(pg)) {
    double hOfParentFromThis =
#ifdef DECOMPOSE_H_INTO_INDEPENDENT_SUBPROBLEMS
        this_node->heurValueOfParentFromThisNode();
#else
        OUR_OWN_nInfinity;
#endif  // DECOMPOSE_H_INTO_INDEPENDENT_SUBPROBLEMS
    current_gap = pg - (hOfParentFromThis - (label + h));
    if (current_gap <= 0.0)
      // this node would be pruned with the h as is. don't need to look ahead.
      return h;
  }

  // the (label + heuristic) of this node is comparable to the
  // h(parent)_from_this_node;
  // if "h(parent)_from_this_node" - "(label + heuristic) of this node" >=
  // PruningGap, this node can/will be pruned.
  // normally (wo lookahead) we would go over augmented[var] and
  // intermediate[var] lists and combine all values.
  // i.e. we would process bucket[var].
  // however, for us here, we would have to look at one level below that in the
  // bucket tree.

  double DH = OUR_OWN_nInfinity;  // next line commented out; see exit() in the
                                  // beginning of this fn.
  //	double DH = _LookaheadResiduals[var].ErrorPerIndSubproblem(assignment,
  // current_gap, subprobH) ;
  return h - DH;
}

void MiniBucketElimLH::noteOrNodeExpansionBeginning(
    int var, vector<val_t> &assignment, SearchNode *search_node) {

  MBLHSubtree* lhHelper = &_Lookahead[var];
  lookahead_subtree_ok_ = false;

  // For sub lookahead subtrees, get the appropriate helper
  if (lhHelper->_IsCopyOfEarlierSubtree) {
    vector<val_t> assignment_backup;
    vector<val_t*> assignment_ptrs;

    int earlier_subtree_var = lhHelper->_IsCopyOfEarlierSubtree->_RootVar;

    // find out which assignments could potentially be modified that need to
    // restored
    int v = var;
    do {
      v = m_pseudotree->getNode(v)->getParent()->getVar(); 
      assignment_backup.push_back(assignment[v]);
      assignment_ptrs.push_back(&assignment[v]);
    } while (v != earlier_subtree_var);

    lookahead_subtree_ok_ = true;
    MBLHSubtree* lhHelperOriginal = &_Lookahead[earlier_subtree_var];
    for (MBLHSubtreeNode* c :
        lhHelper->_IsCopyOfEarlierSubtree->_Children) {
      if (!c->_IsValidForCurrentContext) {
        lookahead_subtree_ok_ = false;
        break;
      }
    }
    if (lookahead_subtree_ok_) {
      ++_nLHcalls[var];
    } else {
      ++_nLHcallsSkipped[var];
    }

    /*
    if (FLAGS_lookahead_always_compute ||
        !need_more_computation_for_copy ||
        _nLHcalls[var] < max_lookahead_trials_ ||
        (FLAGS_lookahead_always_perform_if_better_once &&
          count_better_ordering_[var] > 0) ||
        rand::next_unif() <= lookahead_probability_[var]) {
      ++_nLHcalls[var];
      if (need_more_computation_for_copy) {
        lhHelperOriginal->ComputeHeuristicSubset(assignment,
            lhHelper->_IsCopyOfEarlierSubtree);
        // Restore assignment
        for (int i = 0; i < assignment_backup.size(); ++i) {
          *assignment_ptrs[i] = assignment_backup[i];
        }
      }
      lookahead_subtree_ok_ = true;
    } else {
      ++_nLHcallsSkipped[var];
    }
    */
  } else if (lhHelper->_SubtreeNodes.size() > 0) {
    if (FLAGS_lookahead_always_compute ||
        _nLHcalls[var] < max_lookahead_trials_ ||
        (FLAGS_lookahead_always_perform_if_better_once &&
         count_better_ordering_[var] > 0) ||
        rand::next_unif() <= lookahead_probability_[var]) {
      ++_nLHcalls[var];
      lhHelper->ComputeHeuristic(assignment);
      lookahead_subtree_ok_ = true;
    } else {
      // Invalidate this subtree for it's children
      lhHelper->Invalidate();
      ++_nLHcallsSkipped[var];
    }
  }

}

double MiniBucketElimLH::getHeur(int var, std::vector<val_t> &assignment,
                                 SearchNode *search_node) {
#if defined DEBUG || _DEBUG
  int nSet = 0;
  for (int ii = 0; ii < assignment.size(); ii++) {
    if (assignment[ii] >= 0) nSet++;
  }
#endif

  MBLHSubtree &lhHelper = _Lookahead[var];
  if (0 == lhHelper._SubtreeNodes.size() || !lookahead_subtree_ok_) {
    return MiniBucketElim::getHeur(var, assignment, search_node);
  }
  _Stats._NumNodesLookahead[var] += 1;
#if defined DEBUG || _DEBUG
  // get the pseudo(bucket) tree node; this will give us children so that we can
  // see what they are.
  const PseudotreeNode *n = m_pseudotree->getNode(var);
  const vector<PseudotreeNode *> &children = n->getChildren();
  const int nChildren = children.size();
  for (vector<PseudotreeNode *>::const_iterator it = children.begin();
       it != children.end(); ++it) {
    int child = (*it)->getVar();
    const PseudotreeNode *nChild = m_pseudotree->getNode(child);
    std::vector<MiniBucket> &minibucketsChild = _MiniBuckets[child];
  }
  // get MB value
  double vMB = MiniBucketElim::getHeur(var, assignment, search_node);
  // get LH value; this is exact value
  double vEx = lhHelper.GetHeuristic(assignment);
  double error = vMB - vEx;
  if (OUR_OWN_nInfinity == vEx) {
    // this is fine, whatever the vMB is
  } else if (OUR_OWN_nInfinity == vMB) {
    int bug = 1;
  } else if (error < 0.0) {
    if (error < -1.0e-6) {
      int bug = 1;
    } else {
      // probably floating point error
      int bug = 1;
    }
  }
  // compute local error for all children
  double DH = ELEM_ONE;
  for (vector<PseudotreeNode *>::const_iterator it = children.begin();
       it != children.end(); ++it) {
    int child = (*it)->getVar();
    double dh = getLocalError(child, assignment);  // note : dh should be >= 0.0
    DH OP_TIMESEQ dh;
  }
  int done = 1;
#else
  return lhHelper.GetHeuristic(assignment);
#endif
}

void MiniBucketElimLH::getHeurAll(int var, vector<val_t> &assignment,
                                  SearchNode *search_node,
                                  vector<double> &out) {
  /*
  if (m_options->lookaheadDepth <= 0) {
    MiniBucketElim::getHeurAll(var, assignment, search_node, out);
    return;
  }

  if (_distToClosestDescendantWithLE[var] > m_options->lookaheadDepth) {
    MiniBucketElim::getHeurAll(var, assignment, search_node, out);
    return;  // there is no child with non-0 BucketError within the range of
             // lookahead depth
  }

  MBLHSubtree &lhHelper = _Lookahead[var];
  short i, var_domain_size = m_problem->getDomainSize(var);
  for (i = 0; i < var_domain_size; ++i) {
    assignment[var] = i;
    out[i] = lhHelper.GetHeuristic(assignment);
  }
  */

  val_t old_value = assignment[var];
  val_t var_domain_size = m_problem->getDomainSize(var);

  // always do non lookahead lookups
  MiniBucketElim::getHeurAll(var, assignment, search_node, out);
  /*
  for (int i = 0; i < var_domain_size; ++i) {
    assignment[var] = i;
    out[i] = MiniBucketElim::getHeur(var, assignment, search_node);
  }
  */

  // Check if lookahead is possibly relevant
  MBLHSubtree &lhHelper = _Lookahead[var];
  if (lhHelper._SubtreeNodes.size() > 0 && lookahead_subtree_ok_) {
    if (!FLAGS_lookahead_always_compute) {
      int no_lh_argmax = -1;
      double no_lh_max = ELEM_ZERO;
      int lh_argmax = -1;
      double lh_max = ELEM_ZERO;

      // We decide if they're different by just comparing the
      // argmaxs of the two orderings (results in false negatives)
      for (val_t i = 0; i < var_domain_size; ++i) {
        assignment[var] = i;
        double lh_value = lhHelper.GetHeuristic(assignment);
        if (out[i] > no_lh_max) {
          no_lh_argmax = i;
          no_lh_max = out[i];
        }
        if (lh_value > lh_max) {
          lh_argmax = i;
          lh_max = out[i];
        }

        /*
        if (lh_value - out[i] > 1e-14) {
          cout << "WARNING: lh value is worse than non-lh value: "
            << (lh_value OP_DIVIDE out[i]) << endl;
        }
        */
        out[i] = lh_value;
      }

      // Update count and probability, if ordering is different
      if (no_lh_argmax != lh_argmax) {
        ++count_better_ordering_[var];
        if (!FLAGS_lookahead_fix_probability) {
          lookahead_probability_[var] =
              max(FLAGS_lookahead_min_probability,
                  static_cast<double>(count_better_ordering_[var]) /
                  _nLHcalls[var]);
        }
      }
    } else {
      for (val_t i = 0; i < var_domain_size; ++i) {
        assignment[var] = i;
        out[i] = lhHelper.GetHeuristic(assignment);
      }
    }
  }

  assignment[var] = old_value;
}

// Uses either the _SubtreeError value or computed error functions.
// Current state:
// "static_be" incorporates descendant information
// "sampled_be" incorprates the local error only
// Otherwise, just use the existing search node value.
double MiniBucketElimLH::getOrderingHeur(int var,
                                         std::vector<val_t> &assignment,
                                         SearchNode *n) {
  if (m_options->aobf_subordering == "static_be") {
    return _SubtreeError[var];
  } else if (m_options->aobf_subordering == "sampled_be") {
    //    return _SubtreeError[var];
    // If the error function doesn't exist, assume that it's zero.
    if (!_TrueSlicedBucketErrorFunctions[var]) {
      //      cout << "(no eFn) using bucket error function value: 0"  << endl;
      return 0.0;
    } else {
      //      double v = _BucketErrorFunctions[var]->getValue(assignment);
      double v = _TrueSlicedBucketErrorFunctions[var]->getValue(assignment);
      //      cout << "using bucket error function value: " << v << endl;
      return v;
    }
  } else if (m_options->aobf_subordering == "sampled_st_be") {
    //    return _SubtreeError[var];
    // If the error function doesn't exist, assume that it's zero.
    if (!_SubtreeErrorFunctions[var]) {
      //      cout << "(no eFn) using bucket error function value: 0"  << endl;
      return 0.0;
    } else {
      double v = _SubtreeErrorFunctions[var]->getValue(assignment);
      //      cout << "using bucket error function value: " << v << endl;
      return v;
    }
  } else {
    return n->getOrderingHeurCache()[assignment[var]];
  }
}

double MiniBucketElimLH::getLocalError(int var, vector<val_t> &assignment) {
  // check if number of MBs is 1; if yes, bucket error is 0.
  vector<MiniBucket> &minibuckets = _MiniBuckets[var];
  if (minibuckets.size() <= 1 || 0 == _BucketErrorQuality[var]) return 0.0;

#ifndef DEBUG_BUCKET_ERROR
  if (NULL != _BucketErrorFunctions[var]) {
    double dh = _BucketErrorFunctions[var]->getValue(assignment);
    return dh;
  }
#endif  // DEBUG_BUCKET_ERROR

  int var_domain_size = m_problem->getDomainSize(var);

  // combine MB output FNs
  double tableentryMB = ELEM_ONE;
  for (const MiniBucket &mb : minibuckets) {
    Function *fMB = mb.output_fn();
    if (NULL == fMB) continue;
    tableentryMB OP_TIMESEQ fMB->getValue(assignment);
  }
  if (OUR_OWN_nInfinity == tableentryMB)
    return 0.0;  // it must be that tableentryB is also -inf and error is 0.

  // compute bucket output for the given assignment; this is the exact bucket
  // value, given its functions and assignment.
  vector<double> funVals;
  vector<double> sumVals;
  sumVals.resize(m_problem->getDomainSize(var), ELEM_ONE);
  vector<Function *> &funs_B = _BucketFunctions[var];
  for (Function *fn : funs_B) {
    fn->getValues(assignment, var, funVals);
    for (int i = 0; i < var_domain_size; ++i) {
      sumVals[i] OP_TIMESEQ funVals[i];
    }
  }
  double tableentryB = sumVals.front();
  for (int i = 1; i < var_domain_size; ++i) {
    tableentryB = max(tableentryB, sumVals[i]);
  }

#ifdef DEBUG_BUCKET_ERROR
  if (NULL != _BucketErrorFunctions[var]) {
    double dh = _BucketErrorFunctions[var]->getValue(assignment);
    double dh_ =
        (tableentryMB <= tableentryB) ? 0.0 : (tableentryMB - tableentryB);
    if (fabs(dh - dh_) > 1.0e-6) {
      exit(998);  // error : online/offline versions are different
    }
  }
#endif  // DEBUG_BUCKET_ERROR

  // error must be >= 0 since MB approximation is an upper bound on tableentryB,
  // i.e. Bucket fn value is <= combination of MB output fn values.
  // note, in log-space, tableentryB could be -infinity, but in that case
  // tableentryMB should also be -infinity.
  if (tableentryMB <= tableentryB) return 0.0;  // '<' is an error, '=' is ok.

  return tableentryMB OP_DIVIDE tableentryB;
}

void MiniBucketElimLH::getLocalError(int parent, int var,
                                     vector<val_t> &assignment,
                                     std::vector<double> &out) {
  // check if number of MBs is 1; if yes, bucket error is 0.
  vector<MiniBucket> &minibuckets = _MiniBuckets[var];
  if (minibuckets.size() <= 1 || 0 == _BucketErrorQuality[var]) return;

  vector<double> fun_vals;
  int j, k, parent_domain_size = m_problem->getDomainSize(parent);

  if (NULL != _BucketErrorFunctions[var]) {
    _BucketErrorFunctions[var]->getValues(assignment, parent, fun_vals);
    for (k = 0; k < parent_domain_size; ++k) out[k] OP_DIVIDEEQ fun_vals[k];
    return;
  }

  // we could try to compute directly : out - MB + B, but that would have two
  // problems:
  // 1) we could not catch the rate cases of floating-point errors where MB<B,
  // 2) when MB/B are -inf, the math goes crazy.

  // combine MB output FNs for each value of parent
  vector<double> mb_vals;
  mb_vals.resize((vector<double>::size_type)parent_domain_size, ELEM_ONE);

  for (const MiniBucket &mb : minibuckets) {
    Function *fMB = mb.output_fn();
    if (NULL == fMB) continue;
    fMB->getValues(assignment, parent, fun_vals);
    for (k = 0; k < parent_domain_size; ++k) {
      mb_vals[k] OP_TIMESEQ fun_vals[k];
    }
  }

  // compute bucket output for the given assignment; this is the exact bucket
  // value, given its functions and assignment.
  // note here we have two unassigned variables : parent and child (var), we
  // have to enumerate over one of them.
  vector<Function *> &funs_B = _BucketFunctions[var];
  double parent_original_value = assignment[parent];
  int var_domain_size = m_problem->getDomainSize(var);
  vector<double> sum_vals;
  sum_vals.resize((vector<double>::size_type)var_domain_size);
  for (k = 0; k < parent_domain_size; ++k) {
    assignment[parent] = k;
    for (j = 0; j < var_domain_size; ++j) sum_vals[j] = ELEM_ONE;
    for (Function *fn : funs_B) {
      fn->getValues(assignment, var, fun_vals);
      for (j = 0; j < var_domain_size; ++j) sum_vals[j] OP_TIMESEQ fun_vals[j];
    }
    double tableentryB = sum_vals[0];
    for (j = 1; j < var_domain_size; ++j)
      tableentryB = max(tableentryB, sum_vals[j]);

    // process wrt MB value
    if (mb_vals[k] <= tableentryB) continue;
    out[k] OP_DIVIDEEQ(mb_vals[k] - tableentryB);
  }
  assignment[parent] = parent_original_value;
}

int MiniBucketElimLH::computeLocalErrorTable(
    int var, bool build_table, bool sample_table_if_not_computed,
    double TableMemoryLimitAsNumElementsLog, double &TableSizeLog,
    double &avgError, double &avgExact, Function *&errorFn,
    int64 &nEntriesGenerated) {
  nEntriesGenerated = 0;

  // assumption : each minibucket has its output fn computed
  // algorithm : for each entry in the output table (over 'jointscope'-var)
  // compute the max-product/sum of MB_output_FNs and max-product/sum of all fns
  // in the bucket, and then the different between the two. this should be >= 0.

  // safety check
  if (TableMemoryLimitAsNumElementsLog > 9.0)
    TableMemoryLimitAsNumElementsLog = 9.0;

  avgError = avgExact = DBL_MAX;
  errorFn = NULL;
  TableSizeLog = OUR_OWN_nInfinity;

  int i, k;

  vector<MiniBucket> &minibuckets = _MiniBuckets[var];
  set<int> &jointScope = _BucketScopes[var];

  if (minibuckets.size() <= 1) {
#if defined DEBUG || _DEBUG
    if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
      fprintf(m_options->_fpLogFile,
              "\n   Computing localError for var=%d, nMBs = 1, avg error = 0",
              (int)var);
    }
#endif
    if (m_options->lookahead_use_full_subtree) {
      _BucketErrorQuality[var] = 99;
    } else {
      _BucketErrorQuality[var] = 0;
    }
    avgError = 0.0;
    TableSizeLog = OUR_OWN_nInfinity;
    return 0;
  }
  // as a special case, when table building is not requested and sample size is
  // set to 0, just mark this bucket having actual error, so that LH will use it
  if (!m_options->force_compute_tables && !build_table &&
      TableMemoryLimitAsNumElementsLog <= 0) {
    _BucketErrorQuality[var] = 99;
    avgError = 0.0;
    TableSizeLog = OUR_OWN_nInfinity;
    return 0;
  }

  // make scope of bucket output fn
  set<int> scope = jointScope;
  // remove elimVar from the new scope
  scope.erase(var);
  int scope_size = scope.size();  // new function arity

  // compute table size; do it is log space since sometimes the table size can
  // overflow int64.
  TableSizeLog = 0.0;
  set<int>::const_iterator sit;
  for (sit = scope.begin(); sit != scope.end(); ++sit) {
    double dsLog = log10(m_problem->getDomainSize(*sit));
    if (dsLog < 0.0) {
      _BucketErrorQuality[var] = 0;
      TableSizeLog = OUR_OWN_nInfinity;
      avgError = 0.0;
      return 0;
    }
    TableSizeLog += dsLog;
  }
  if (TableSizeLog < 0.0) {  // this should not happen; we check just in case.
    _BucketErrorQuality[var] = 0;
    avgError = 0.0;
    TableSizeLog = OUR_OWN_nInfinity;
    return 0;
  }
  if (TableSizeLog > TableMemoryLimitAsNumElementsLog) {  // table would be too
                                                          // large; will not fit
                                                          // in memory.
    if (!sample_table_if_not_computed) {
      // just mark this bucket having actual error, so that LH will use it
      _BucketErrorQuality[var] = 99;
      avgError = 0.0;
      return 1;
    }
    build_table = false;
  }

  // construct vector of scope domains; compute new table size and collect
  // domain sizes
  vector<val_t> domains;
  domains.reserve(scope_size);
  int64 TableSize = 1;
  for (sit = scope.begin(); sit != scope.end(); ++sit) {
    TableSize *= ((int64)m_problem->getDomainSize(*sit));
    domains.push_back(m_problem->getDomainSize(*sit));
  }
  size_t bucket_var_domain_size = m_problem->getDomainSize(var);
  if (TableSize <= 0) {  // this should not happen; we check just in case.
    _BucketErrorQuality[var] = 0;
    avgError = 0.0;
    TableSizeLog = OUR_OWN_nInfinity;
    return 0;
  }

  // collect all FNs
  vector<Function *> &funs_B = _BucketFunctions[var];
  vector<Function *> funs_MB;
  for (const MiniBucket &mini_bucket : minibuckets) {
    Function *fMB = mini_bucket.output_fn();
    if (NULL == fMB) continue;
    funs_MB.push_back(fMB);
  }

  vector<int> scopeB;
  int n = 0;
  val_t *tuple = NULL;
  vector<vector<val_t *>> idxMapB;
  computeMBOutFnArgsVectorPtrMap(var, funs_B, scopeB, n, tuple, idxMapB);
  vector<int> scopeMB;
  vector<vector<val_t *>> idxMapMB;
  computeMBOutFnArgsVectorPtrMap(INT_MAX, funs_MB, scopeMB, n, tuple, idxMapMB);
  // size of tuple is n+1

  // safety check : scopes must be the same
  if (scopeB.size() != scopeMB.size())  // scope of all bucket FNs - var should
                                        // be the same as (the union of) scopes
                                        // of MB output FNs.
    return 1;
  if (scopeB.size() != scope.size()) return 1;
  if (int(scope.size()) != n) return 1;

  if (_Stats._EnforcedPseudoWidth < scope_size)
    _Stats._EnforcedPseudoWidth = scope_size;

  // we will iterate over all combinations of new fn scope value stored here
  for (i = n - 2; i >= 0; --i) tuple[i] = 0;
  tuple[n - 1] = -1;

  double *newTable = NULL;
  if (build_table) {
    try {
      newTable = new double[TableSize];
    }
    catch (...) {
    }
    if (NULL == newTable) return 1;
  }

#if defined DEBUG || _DEBUG
  size_t nBadErrorValues = 0;
#endif

#if defined DEBUG || _DEBUG
  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    fprintf(
        m_options->_fpLogFile,
        "\n   MiniBucketElimLH::computeLocalErrorTable var=%d tablesize=%lld",
        (int)var, (int64)TableSize);
  }
  printf("\n   MiniBucketElimLH::computeLocalErrorTable var=%d tablesize=%lld",
         (int)var, (int64)TableSize);
#endif

  int64 nEntries_both_inf = 0, nEntries_B_inf = 0, nEntries_non_inf = 0;
  double avgExact_non_inf = 0.0, avgError_non_inf = 0.0;
  double errorAbsMin = OUR_OWN_pInfinity, errorAbsMax = OUR_OWN_nInfinity;

  // enumerate all new fn scope combinations
  double e, numErrorItems = 0.0;
  avgError = avgExact = 0.0;
  int64 nEntriesRequested = pow(10.0, TableMemoryLimitAsNumElementsLog);
  if (nEntriesRequested < 1024) {
    nEntriesRequested = 1024;  // enumarate small tables completely
  }

  // number of entries we will enumerate (whether sampling or not)
  nEntriesRequested = min(TableSize, nEntriesRequested);

  double sample_coverage = 0.0;
  bool enumerate_table = build_table || nEntriesRequested >= TableSize;
  if (enumerate_table) {
    ++nFNsBEexact;
  } else {
    ++nFNsBEsampled;
  }

  double total_sample_weight_noninf = 0.0;

  //  cout << "entries requested: " << nEntriesRequested << endl;
  // printf("\nBUCKET ERROR TABLE for var=%d", (int)var) ;
  for (int64 j = 0; j < nEntriesRequested; j++) {
    ++nEntriesGenerated;
    if (enumerate_table) {
      // find next combination
      for (i = n - 1; i >= 0; i--) {
        if (++tuple[i] < domains[i]) break;
        tuple[i] = 0;
      }
    } else {
      // generate random tuple
      for (i = n - 1; i >= 0; i--) {
        int k = m_problem->getDomainSize(scopeB[i]);
        tuple[i] = rand::next(k);
      }
    }

    // enumerate over all bucket var values; combine all bucket FNs
    double tableentryB = ELEM_ZERO, zB;
    for (tuple[n] = 0; tuple[n] < int(bucket_var_domain_size); tuple[n]++) {
      zB = ELEM_ONE;
      for (k = 0; k < int(funs_B.size()); ++k)
        zB OP_TIMESEQ funs_B[k]->getValuePtr(idxMapB[k]);
      tableentryB = max(tableentryB, zB);
    }

    // combine MB output FNs
    double tableentryMB = ELEM_ONE;
    for (k = 0; k < int(funs_MB.size()); k++)
      tableentryMB OP_TIMESEQ funs_MB[k]->getValuePtr(idxMapMB[k]);

    double sample_weight =
        FLAGS_bee_importance_sampling ? pow(10.0, tableentryMB) : 1.0;

    e = tableentryMB - tableentryB;
    if (e >= 0 && e < OUR_OWN_pInfinity && tableentryB != OUR_OWN_nInfinity) {
      total_sample_weight_noninf += sample_weight;
      avgError_non_inf += sample_weight * e;
      avgExact_non_inf += sample_weight * tableentryB;
    }

    /*
    // compute numbers of special cases
    if (OUR_OWN_nInfinity == tableentryB) {
      if (OUR_OWN_nInfinity == tableentryMB)
        nEntries_both_inf++;
      else
        nEntries_B_inf++;
    } else {
      avgExact_non_inf += sample_weight * tableentryB;
      total_sample_weight_error_noninf += sample_weight;
      nEntries_non_inf++;
    }

    if (tableentryMB <= tableentryB) {  // '<' is an error, '=' is ok.
      e = 0.0;
#if defined DEBUG || _DEBUG
      if (tableentryMB < tableentryB) nBadErrorValues++;
#endif
    } else { // note tableentryB may be -infinity.
      e = tableentryMB - tableentryB;
    }
    e *= sample_weight;
    */

    // at this point, it should be that e >= 0

    numErrorItems += 1.0;
    avgError += e;
    avgExact += tableentryB;
    if (e < errorAbsMin) errorAbsMin = e;
    if (e > errorAbsMax) errorAbsMax = e;
    if (NULL != newTable) newTable[j] = e;
  }
  sample_coverage = 100.0 * ((double)nEntriesGenerated) / ((double)TableSize);

  if (nEntriesGenerated <= 0) {
    // we have no data; leave _BucketErrorQuality[] as is (-1 most likely).
    return 0;
  }

  // avgExact_non_inf/avgError_non_inf are avg in case when neither MB/B value
  // is -infinity.
  if (total_sample_weight_noninf > 0) {
    avgError_non_inf /= total_sample_weight_noninf;
    avgExact_non_inf /= total_sample_weight_noninf;
  }
  // rel_error is relative error in case when neither MB/B value is -infinity.
  double rel_error = fabs(avgExact_non_inf) > 0.0
                         ? fabs(100.0 * avgError_non_inf / avgExact_non_inf)
                         : -1.0;
  _BucketError_Rel[var] = rel_error;

  if (numErrorItems > 0.0) {
    avgError = avgError / numErrorItems;
    avgExact = avgExact / numErrorItems;
    if (total_sample_weight_noninf > 0.0) {
      _BucketError_AbsAvg[var] = avgError_non_inf;
    } else {
      _BucketError_AbsAvg[var] = OUR_OWN_nInfinity;
    }
    _BucketError_AbsMin[var] = errorAbsMin;
    _BucketError_AbsMax[var] = errorAbsMax;

    if (m_options->_fpLogFile) {
      fprintf(m_options->_fpLogFile,
              "\n   Computing localError for var=%d avg abs = %g "
              "(min=%g;max=%g); avg rel = %g",
              (int)var, (double)_BucketError_AbsAvg[var],
              (double)_BucketError_AbsMin[var],
              (double)_BucketError_AbsMax[var], (double)rel_error);
    }
  } else {
    if (m_options->_fpLogFile) {
      fprintf(m_options->_fpLogFile,
              "\n   Computing localError for var=%d no error items", (int)var);
    }
    if (build_table) build_table = false;
  }

  double threshold = DBL_MIN;
  if (NULL != m_options) threshold = m_options->lookahead_LE_IgnoreThreshold;
  if (threshold < DBL_MIN) threshold = DBL_MIN;
  // if there is substantial error, mark it as such.
  // if avgError is 0, don't build table. note avgError could be infinity (in
  // case of log space representation).
  if (nEntries_B_inf > 0 || rel_error > threshold) {
    // this table has substantial bucket error; we should be lookahead.
    _BucketErrorQuality[var] = 2;
  } else if (avgError <= DBL_MIN && enumerate_table) {
    _BucketErrorQuality[var] = 0;
    build_table = false;
  } else if (avgError <= DBL_MIN && nEntriesGenerated > 0) {
    _BucketErrorQuality[var] = 0;
    build_table = false;
  } else {
    _BucketErrorQuality[var] = 1;
  }

  if (build_table && NULL != newTable) {
    errorFn = new FunctionBayes(-var, m_problem, scope, newTable, TableSize);
    if (NULL != errorFn) newTable = NULL;  // table belongs to errorFn
  }

#if defined DEBUG || _DEBUG
  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    //		double rel_error = fabs(avgExact_non_inf) > 0.0 ? fabs(100.0 *
    // avgError_non_inf / avgExact_non_inf) : DBL_MAX;
    fprintf(m_options->_fpLogFile,
            "\n   Computing localError for var=%d (depth=%d), nMBs = %d,"
            " avg error = %g(%g), avg exact = %g(%g), tablesize = %lld entries;"
            "nSpecialCases=%lld/%lld/%lld",
            (int)var, (int)_Depth[var], (int)minibuckets.size(),
            (double)avgError, (double)avgError_non_inf, (double)avgExact,
            (double)avgExact_non_inf, (int64)TableSize,
            (int64)nEntries_both_inf, (int64)nEntries_B_inf,
            (int64)nEntries_non_inf);
    if (rel_error < 1.0e+100)
      fprintf(m_options->_fpLogFile, ", rel avg error = %g%c",
              (double)rel_error, '%');
  }
#endif

  //	_Stats._LEMemorySizeMB += TableSize * sizeof(double) / (1024.0 * 1024);
  _Stats._LEMemorySizeMB +=
      nEntriesGenerated * sizeof(double) / (1024.0 * 1024);

  if (NULL != newTable) delete[] newTable;
  if (NULL != tuple) delete[] tuple;

  return 0;
}

// Generates error functions based on a passed in scope
int MiniBucketElimLH::computeLocalErrorTableSlice(
    int var, const set<int> &output_scope,
    double TableMemoryLimitAsNumElementsLog, double &TableSizeLog,
    double &avgError, double &avgExact, Function *&errorFn,
    int64 &nEntriesGenerated) {
  nEntriesGenerated = 0;

  // assumption : each minibucket has its output fn computed
  // algorithm : for each entry in the output table (over 'jointscope'-var)
  // compute the max-product/sum of MB_output_FNs and max-product/sum of all fns
  // in the bucket, and then the different between the two. this should be >= 0.

  // safety check: slices should take memory no more than the i-bound.
  if (output_scope.size() > m_ibound) {
    assert(false);
  }

  avgError = avgExact = DBL_MAX;
  errorFn = NULL;
  TableSizeLog = OUR_OWN_nInfinity;

  vector<MiniBucket> &minibuckets = _MiniBuckets[var];
  set<int> &jointScope = _BucketScopes[var];

  // safety check
  assert(isSubset(output_scope, jointScope));

  // make scope of bucket output fn
  set<int> scope = jointScope;
  set<int> scope_slice = output_scope;
  // remove elimVar from the new scope
  scope.erase(var);
  scope_slice.erase(var);

  // construct vector of scope domains; compute new table size and collect
  // domain sizes
  vector<val_t> slice_domains;
  slice_domains.reserve(scope_slice.size());

  // Should be safe to do this in linear space since the slice is no larger
  // than the i-bound
  int64 TableSize = 1;
  set<int>::const_iterator sit;
  for (sit = scope_slice.begin(); sit != scope_slice.end(); ++sit) {
    TableSize *= ((int64)m_problem->getDomainSize(*sit));
    slice_domains.push_back(m_problem->getDomainSize(*sit));
  }

  // build a dummy function of zeros if trivially no error
  if (minibuckets.size() <= 1) {
    double *newTable = new double[TableSize];
    for (int j = 0; j < TableSize; ++j) {
      newTable[j] = 0.0;
    }
    avgError = 0.0;
    errorFn =
        new FunctionBayes(-var, m_problem, scope_slice, newTable, TableSize);
    return 0;
  }

  TableSizeLog = log10(TableSize);

  double sampling_space_available_log =
      TableMemoryLimitAsNumElementsLog - TableSizeLog;
  assert(sampling_space_available_log >= 0.0);
  int64 times_to_sample = pow(10.0, sampling_space_available_log);

  // Set times to sample such that it is as large as possible but under
  // the TableMemoryLimitAsNumElementsLog
  size_t bucket_var_domain_size = m_problem->getDomainSize(var);

  // collect all FNs
  vector<Function *> &funs_B = _BucketFunctions[var];
  vector<Function *> funs_MB;
  for (const MiniBucket &mini_bucket : minibuckets) {
    Function *fMB = mini_bucket.output_fn();
    if (NULL == fMB) continue;
    funs_MB.push_back(fMB);
  }

  int n = 0;
  val_t *tuple = NULL;

  vector<int> scopeB;
  vector<vector<val_t *>> idxMapB;
  computeMBOutFnArgsVectorPtrMap(var, funs_B, scopeB, n, tuple, idxMapB);

  vector<int> scopeMB;
  vector<vector<val_t *>> idxMapMB;
  computeMBOutFnArgsVectorPtrMap(INT_MAX, funs_MB, scopeMB, n, tuple, idxMapMB);
  // size of tuple is n+1

  // safety check : scopes must be the same
  // scope of all bucket FNs - var should
  // be the same as (the union of) scopes
  // of MB output FNs.
  assert(scopeB.size() == scopeMB.size());
  assert(scopeB.size() == scope.size());

  // Indexes into tuple
  vector<val_t *> tuple_slice;
  vector<val_t *> tuple_sample;

  vector<int> scope_sample;

  for (int i = 0; i < scopeB.size(); ++i) {
    if (ContainsKey(scope_slice, scopeB[i])) {
      tuple_slice.push_back(&tuple[i]);
    } else {
      tuple_sample.push_back(&tuple[i]);
      scope_sample.push_back(scopeB[i]);
    }
  }

  int64 sample_cardinality = 1;
  vector<val_t> sample_domains;
  sample_domains.reserve(scope_sample.size());
  for (int v : scope_sample) {
    sample_cardinality *= ((int64)m_problem->getDomainSize(v));
    sample_domains.push_back(m_problem->getDomainSize(v));
  }

  // Readjust time_to_sample to not exceed the scope
  times_to_sample = min(times_to_sample, sample_cardinality);
  /*
  cout << "var: " << var << ", ";
  cout << "Will sample " << times_to_sample << " times for each entry" << endl;
  */

  // we will iterate over all combinations of new fn scope value stored here
  for (int i = n - 1; i >= 0; --i) tuple[i] = 0;

  double *newTable = NULL;
  try {
    newTable = new double[TableSize];
  }
  catch (...) {
    cout << "out of memory" << endl;
  }
  assert(newTable);
//  if (NULL == newTable) return 1;

#if defined DEBUG || _DEBUG
  size_t nBadErrorValues = 0;
#endif

#if defined DEBUG || _DEBUG
  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    fprintf(
        m_options->_fpLogFile,
        "\n   MiniBucketElimLH::computeLocalErrorTable var=%d tablesize=%lld",
        (int)var, (int64)TableSize);
  }
  printf("\n   MiniBucketElimLH::computeLocalErrorTable var=%d tablesize=%lld",
         (int)var, (int64)TableSize);
#endif

  int64 nEntries_both_inf = 0, nEntries_B_inf = 0, nEntries_non_inf = 0;
  double avgExact_non_inf = 0.0, avgError_non_inf = 0.0;
  double errorAbsMin = OUR_OWN_pInfinity, errorAbsMax = OUR_OWN_nInfinity;
  double overall_total_sample_weight_noninf = 0.0;

  // enumerate all new fn scope combinations
  double numErrorItems = 0.0;
  avgError = avgExact = 0.0;
  bool enumerate_table = times_to_sample >= sample_cardinality;
  if (enumerate_table) {
    ++nFNsBEexact;
  } else {
    ++nFNsBEsampled;
  }
  int64 nEntriesRequested = pow(10.0, TableMemoryLimitAsNumElementsLog);
  if (nEntriesRequested < 1024)
    nEntriesRequested = 1024;  // enumarate small tables completely

  // number of entries we will enumerate (whether sampling or not)
  nEntriesRequested = min(TableSize, nEntriesRequested);

  double sample_coverage = 0.0;
  //   printf("\nBUCKET ERROR TABLE for var=%d", (int)var) ;
  bool slice_inc_done = false;
  //  cout << "Times to sample: " << times_to_sample << endl;
  for (int64 j = 0; j < nEntriesRequested; j++) {

    double e_sampled_avg = 0.0;
    double exact_noninf_sampled_avg = 0.0;
    int64 num_rejected_samples = 0;
    bool enumerate_done = false;
    double total_sample_weight_noninf = 0.0;
    for (int64 ks = 0; ks < times_to_sample; ++ks) {
      ++nEntriesGenerated;
      if (!enumerate_table) {
        // get random tuple for non-slice
        for (int i = tuple_sample.size() - 1; i >= 0; i--) {
          int d_size = m_problem->getDomainSize(scope_sample[i]);
          *tuple_sample[i] = rand::next(d_size);
        }
      }
      // enumerate over all bucket var values; combine all bucket FNs
      double tableentryB = ELEM_ZERO, zB;
      for (tuple[n] = 0; tuple[n] < int(bucket_var_domain_size); tuple[n]++) {
        zB = ELEM_ONE;
        for (int k = 0; k < int(funs_B.size()); ++k) {
          zB OP_TIMESEQ funs_B[k]->getValuePtr(idxMapB[k]);
        }
        tableentryB = max(tableentryB, zB);
      }

      // combine MB output FNs
      double tableentryMB = ELEM_ONE;
      for (int k = 0; k < int(funs_MB.size()); k++)
        tableentryMB OP_TIMESEQ funs_MB[k]->getValuePtr(idxMapMB[k]);

      double sample_weight =
          FLAGS_bee_importance_sampling ? pow(10.0, tableentryMB) : 1.0;

      double e = tableentryMB - tableentryB;
      if (e >= 0 && e < OUR_OWN_pInfinity && tableentryB != OUR_OWN_nInfinity) {
        avgError_non_inf += sample_weight * e;
        avgExact_non_inf += sample_weight * tableentryB;

        total_sample_weight_noninf += sample_weight;
        e_sampled_avg += sample_weight * e;
        exact_noninf_sampled_avg += sample_weight * tableentryB;
      }

      /*
      // compute numbers of special cases
      if (OUR_OWN_nInfinity == tableentryB) {
        if (OUR_OWN_nInfinity == tableentryMB)
          nEntries_both_inf++;
        else
          nEntries_B_inf++;
      } else {
        avgExact_non_inf += sample_weight * tableentryB;
        exact_noninf_sampled_avg += sample_weight * tableentryB;
        total_sample_weight_exact_noninf += sample_weight;
        nEntries_non_inf++;
      }

      double e = 0.0;
      if (tableentryMB < tableentryB){
#ifdef DEBUG
        nBadErrorValues++;
#endif
      } else { // note tableentryB may be -infinity.
        e = tableentryMB - tableentryB;
        // If both are inf, then there is no error
        if (std::isinf(tableentryMB) && std::isinf(tableentryB)) {
          e = 0.0;
        }
      }
      e *= sample_weight;

      // at this point, it should be that e >= 0
      if (e < OUR_OWN_pInfinity) {
        avgError_non_inf += e;
        e_sampled_avg += e;
        total_sample_weight_error_noninf += sample_weight;
      } else { // reject inf samples
        ++num_rejected_samples;
      }
      */
      if (e < errorAbsMin) errorAbsMin = e;
      if (e > errorAbsMax) errorAbsMax = e;

      avgExact += tableentryB;
      avgError += e;
      numErrorItems += 1.0;
      overall_total_sample_weight_noninf += total_sample_weight_noninf;

      if (enumerate_table) {
        enumerate_done = !IdxMapIncrement(tuple_sample, sample_domains);
      }
    }
    assert(!enumerate_table || enumerate_done);
    e_sampled_avg /= total_sample_weight_noninf;
    exact_noninf_sampled_avg /= total_sample_weight_noninf;
    // make relative error?
    if (FLAGS_aobf_subordering_use_relative_error) {
      e_sampled_avg = fabs(exact_noninf_sampled_avg) > 0.0
                          ? fabs(100 * e_sampled_avg / exact_noninf_sampled_avg)
                          : 0.0;
    }
    if (NULL != newTable) newTable[j] = e_sampled_avg;

    slice_inc_done = !IdxMapIncrement(tuple_slice, slice_domains);
  }

  if (nEntriesGenerated <= 0) {
    // we have no data; leave _BucketErrorQuality[] as is (-1 most likely).
    return 0;
  }

  // avgExact_non_inf/avgError_non_inf are avg in case when neither MB/B value
  // is -infinity.
  if (overall_total_sample_weight_noninf > 0) {
    avgError_non_inf /= overall_total_sample_weight_noninf;
    avgExact_non_inf /= overall_total_sample_weight_noninf;
  }
  // rel_error is relative error in case when neither MB/B value is -infinity.
  double rel_error = fabs(avgExact_non_inf) > 0.0
                         ? fabs(100.0 * avgError_non_inf / avgExact_non_inf)
                         : -1.0;
  _BucketError_Rel[var] = rel_error;

  if (numErrorItems > 0.0) {
    avgError = avgError / numErrorItems;
    avgExact = avgExact / numErrorItems;
    if (nEntries_non_inf > 0) {
      _BucketError_AbsAvg[var] = avgError_non_inf;
    } else {
      _BucketError_AbsAvg[var] = OUR_OWN_nInfinity;
    }
    _BucketError_AbsMin[var] = errorAbsMin;
    _BucketError_AbsMax[var] = errorAbsMax;

    if (m_options->_fpLogFile) {
      fprintf(m_options->_fpLogFile,
              "\n   Computing localError for var=%d avg abs = %g "
              "(min=%g;max=%g); avg rel = %g",
              (int)var, (double)_BucketError_AbsAvg[var],
              (double)_BucketError_AbsMin[var],
              (double)_BucketError_AbsMax[var], (double)rel_error);
    }
  } else {
    if (m_options->_fpLogFile) {
      fprintf(m_options->_fpLogFile,
              "\n   Computing localError for var=%d no error items", (int)var);
    }
  }

  double threshold = DBL_MIN;
  if (NULL != m_options) threshold = m_options->lookahead_LE_IgnoreThreshold;
  if (threshold < DBL_MIN) threshold = DBL_MIN;
  // if there is substantial error, mark it as such.
  // if avgError is 0, don't build table. note avgError could be infinity (in
  // case of log space representation).
  if (nEntries_B_inf > 0 || rel_error > threshold) {
    // this table has substantial bucket error; we should be lookahead.
    _BucketErrorQuality[var] = 2;
  } else if (avgError <= DBL_MIN && nEntriesGenerated > 0) {
    _BucketErrorQuality[var] = 0;
  } else {
    _BucketErrorQuality[var] = 1;
  }

  if (newTable) {
    errorFn =
        new FunctionBayes(-var, m_problem, scope_slice, newTable, TableSize);
    if (NULL != errorFn) newTable = NULL;  // table belongs to errorFn
  }

#if defined DEBUG || _DEBUG
  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    //		double rel_error = fabs(avgExact_non_inf) > 0.0 ? fabs(100.0 *
    // avgError_non_inf / avgExact_non_inf) : DBL_MAX;
    fprintf(m_options->_fpLogFile,
            "\n   Computing localError for var=%d (depth=%d), nMBs = %d,"
            " avg error = %g(%g), avg exact = %g(%g), tablesize = %lld entries;"
            "nSpecialCases=%lld/%lld/%lld",
            (int)var, (int)_Depth[var], (int)minibuckets.size(),
            (double)avgError, (double)avgError_non_inf, (double)avgExact,
            (double)avgExact_non_inf, (int64)TableSize,
            (int64)nEntries_both_inf, (int64)nEntries_B_inf,
            (int64)nEntries_non_inf);
    if (rel_error < 1.0e+100)
      fprintf(m_options->_fpLogFile, ", rel avg error = %g%c",
              (double)rel_error, '%');
  }
#endif

  //	_Stats._LEMemorySizeMB += TableSize * sizeof(double) / (1024.0 * 1024);
  _Stats._LEMemorySizeMB +=
      nEntriesGenerated * sizeof(double) / (1024.0 * 1024);

  if (NULL != newTable) delete[] newTable;
  if (NULL != tuple) delete[] tuple;

  return 0;
}

int MiniBucketElimLH::computeLocalErrorTables(
    bool build_tables, double TotalMemoryLimitAsNumElementsLog,
    double TableMemoryLimitAsNumElementsLog) {
  vector<int> elimOrder;    // will hold dfs order
  findDfsOrder(elimOrder);  // computes dfs ordering of relevant subtree

  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    fprintf(m_options->_fpLogFile,
            "\n\nWILL COMPUTE LOCAL ERROR for each bucket ..."
            " error=(MB value - B value); TotalMemory=%g, TableMemory=%g",
            TotalMemoryLimitAsNumElementsLog, TableMemoryLimitAsNumElementsLog);
    fflush(m_options->_fpLogFile);
  }
  printf(
      "\n\nWILL COMPUTE LOCAL ERROR for each bucket ..."
      "error=(MB value - B value); TotalMemory=%g, TableMemory=%g",
      TotalMemoryLimitAsNumElementsLog, TableMemoryLimitAsNumElementsLog);

  deleteLocalErrorFNs();
  _BucketErrorFunctions.resize(m_problem->getN());
  _TrueSlicedBucketErrorFunctions.resize(m_problem->getN());
  _BucketErrorFnTableSizes_Total = _BucketErrorFnTableSizes_Precomputed =
      _BucketErrorFnTableSizes_Ignored =
          -DBL_MIN;  // don't make -infinity; math will blow up.
  _nBucketsWithNonZeroBucketError = _nBucketsWithMoreThan1MB = 0;

  // compute total of all bucket error table sizes
  for (vector<int>::reverse_iterator itV = elimOrder.rbegin();
       itV != elimOrder.rend(); ++itV) {
    int v = *itV;  // this is the variable being eliminated
    vector<MiniBucket> &minibuckets = _MiniBuckets[v];
    if (minibuckets.size() <= 1) continue;
    ++_nBucketsWithMoreThan1MB;
    set<int> &jointScope = _BucketScopes[v];
    double TableSize = 0.0;
    for (set<int>::const_iterator sit = jointScope.begin();
         sit != jointScope.end(); ++sit) {
      int u = *sit;
      if (u == v) continue;
      double dsLog = log10(m_problem->getDomainSize(u));
      if (dsLog < 0) {
        TableSize = -1.0;
        break;
      }
      TableSize += dsLog;
    }
    if (TableSize >= 0.0)
      _BucketErrorFnTableSizes_Total =
          _BucketErrorFnTableSizes_Total +
          log10(1.0 + pow(10.0, TableSize - _BucketErrorFnTableSizes_Total));
  }

  if (NULL != m_options ? NULL != m_options->_fpLogFile : false)
    fprintf(m_options->_fpLogFile,
            "\n   BucketErrorFnTableSizes total = %g, total_memory_limit = %g, "
            "total_memory_limit = %g",
            (double)_BucketErrorFnTableSizes_Total,
            (double)TotalMemoryLimitAsNumElementsLog,
            (double)TableMemoryLimitAsNumElementsLog);
  printf(
      "\n   BucketErrorFnTableSizes total = %g, total_memory_limit = %g, "
      "total_memory_limit = %g\n",
      (double)_BucketErrorFnTableSizes_Total,
      (double)TotalMemoryLimitAsNumElementsLog,
      (double)TableMemoryLimitAsNumElementsLog);

  int64 nTotalEntriesGenerated = 0;
  for (vector<int>::reverse_iterator itV = elimOrder.rbegin();
       itV != elimOrder.rend(); ++itV) {
    int v = *itV;  // this is the variable being eliminated
    _BucketErrorQuality[v] = -1;
    double table_space_left =
        (TotalMemoryLimitAsNumElementsLog > 0.0 &&
         TotalMemoryLimitAsNumElementsLog >
             _BucketErrorFnTableSizes_Precomputed)
            ? TotalMemoryLimitAsNumElementsLog +
                  log10(1 - pow(10.0, _BucketErrorFnTableSizes_Precomputed -
                                          TotalMemoryLimitAsNumElementsLog))
            : OUR_OWN_nInfinity;
    double table_size_actual_limit =
        table_space_left < TableMemoryLimitAsNumElementsLog
            ? table_space_left
            : TableMemoryLimitAsNumElementsLog;
    Function *errorFn = NULL;
    double avgError, E;
    double tableSize = -1.0;
    bool do_sample = true;
    bool build_table = build_tables;
    int64 nEntriesGenerated = 0;
    if (table_size_actual_limit <= 0) {
      build_table = false;
      table_size_actual_limit = TableMemoryLimitAsNumElementsLog;
    }
#ifdef NO_LH_PREPROCESSING
    build_table = do_sample = false;
    table_size_actual_limit = -DBL_MIN;
#endif  //
    if (m_options->lookahead_use_full_subtree) {
      do_sample = false;
    }

    if (m_options->aobf_subordering == "sampled_be" ||
        m_options->aobf_subordering == "sampled_st_be") {
      // Slice option: remove variables from the bucket scope until
      // the number of variables is less than or equal to the specified
      // flag. Currently arbitrarily making it so we use 2 fewer variables
      // than the i-bound to guarantee less memory usage.
      //
      // We also need to continue removing variables if the sliced scope
      // results in a table with more entries than the table limit parameter.
      double current_table_size_log = 0.0;
      set<int> output_scope(_BucketScopes[v]);
      cout << "bvar:" << v << " | " << _BucketScopes[v] << endl;
      for (int v : output_scope) {
        current_table_size_log += log10(m_problem->getDomainSize(v));
      }
      int target_scope_size =
          min(m_options->bee_slice_sample_scope_size, m_ibound);

      const vector<int> &elim_order = m_pseudotree->getElimOrder();

      // We either keep the closest variables or the farthest.
      // we must keep the bucket variable however.
      if (m_options->bee_slice_sample_closest_first) {
        for (auto itV = elim_order.rbegin(); itV != elim_order.rend(); ++itV) {
          if (output_scope.size() <= target_scope_size &&
              current_table_size_log <= table_size_actual_limit) {
            break;
          }
          if (*itV != v && output_scope.erase(*itV)) {
            current_table_size_log -= log10(m_problem->getDomainSize(*itV));
          }
        }
      } else {  // farthest first
        for (auto itV = elim_order.begin(); itV != elim_order.end(); ++itV) {
          if (output_scope.size() <= target_scope_size &&
              current_table_size_log <= table_size_actual_limit) {
            break;
          }
          if (*itV != v && output_scope.erase(*itV)) {
            current_table_size_log -= log10(m_problem->getDomainSize(*itV));
          }
        }
      }
      computeLocalErrorTableSlice(v, output_scope, table_size_actual_limit,
                                  tableSize, avgError, E, errorFn,
                                  nEntriesGenerated);
    } else {
      computeLocalErrorTable(v, build_table, do_sample, table_size_actual_limit,
                             tableSize, avgError, E, errorFn,
                             nEntriesGenerated);
    }
    nTotalEntriesGenerated += nEntriesGenerated;
    _BucketErrorFunctions[v] = errorFn;
    if (_BucketErrorQuality[v] > 1) _nBucketsWithNonZeroBucketError++;
    if (NULL != errorFn ? NULL != errorFn->getTable() : false) {
      double *table = errorFn->getTable();
      if (tableSize > 0)
        _BucketErrorFnTableSizes_Precomputed =
            _BucketErrorFnTableSizes_Precomputed +
            log10(1.0 +
                  pow(10.0, tableSize - _BucketErrorFnTableSizes_Precomputed));
      // compute standard deviation
      double stdDev = -DBL_MAX;
      size_t ts = errorFn->getTableSize();
      double sum = 0.0;
      for (size_t ti = 0; ti < ts; ti++) {
        double d = avgError - table[ti];
        double ds = d * d;
        sum += ds;
      }
#if defined DEBUG || _DEBUG
      if (isnan(sum)) {
        int bad = 1;
      }
#endif
      double variance = sum / ts;
      stdDev = sqrt(variance);
#if defined DEBUG || _DEBUG
      if (NULL != m_options ? NULL != m_options->_fpLogFile : false)
        fprintf(m_options->_fpLogFile, ", stdDev = %g", (double)stdDev);
#endif
    } else if (tableSize > 0) {
      _BucketErrorFnTableSizes_Ignored =
          _BucketErrorFnTableSizes_Ignored +
          log10(1.0 + pow(10.0, tableSize - _BucketErrorFnTableSizes_Ignored));
      if (_BucketErrorQuality[v] > 0) {
        // this would be bad; something is wrong. maybe table was too large.
      }
    }
  }

  if (NULL != m_options ? NULL != m_options->_fpLogFile : false) {
    fprintf(m_options->_fpLogFile,
            "\n   BucketErrorFnTableSizes (precomputed/ignored/total) = "
            "%g/%g/%g entries; nTotalEntriesGenerated=%lld",
            (double)_BucketErrorFnTableSizes_Precomputed,
            (double)_BucketErrorFnTableSizes_Ignored,
            (double)_BucketErrorFnTableSizes_Total,
            (int64)nTotalEntriesGenerated);
    fprintf(
        m_options->_fpLogFile,
        "\n   nBucketsWithNonZeroBucketError (nMB>1/total) = %lld (%lld/%lld)",
        (int64)_nBucketsWithNonZeroBucketError, (int64)_nBucketsWithMoreThan1MB,
        (int64)m_problem->getN());
    fprintf(m_options->_fpLogFile,
            "\n   BE computation : nFNsBEexact=%d nFNsBEsampled=%d",
            nFNsBEexact, nFNsBEsampled);
    fprintf(m_options->_fpLogFile, "\n");
  }
  printf(
      "\n   BucketErrorFnTableSizes (precomputed/ignored/total) = %g/%g/%g "
      "entries; nTotalEntriesGenerated=%lld",
      (double)_BucketErrorFnTableSizes_Precomputed,
      (double)_BucketErrorFnTableSizes_Ignored,
      (double)_BucketErrorFnTableSizes_Total, (int64)nTotalEntriesGenerated);
  printf("\n   nBucketsWithNonZeroBucketError (nMB>1/total) = %lld (%lld/%lld)",
         (int64)_nBucketsWithNonZeroBucketError,
         (int64)_nBucketsWithMoreThan1MB, (int64)m_problem->getN());
  printf("\n   BE computation : nFNsBEexact=%d nFNsBEsampled=%d", nFNsBEexact,
         nFNsBEsampled);
  printf("\n");

  if (m_options->_fpLogFile) {
    fprintf(m_options->_fpLogFile, "\nBucketAbsError:\n");
    fprintf(m_options->_fpLogFile, "%lld ", (int64)m_problem->getN());
    for (auto err : _BucketError_AbsAvg) {
      fprintf(m_options->_fpLogFile, " %.3f", err);
    }
    fprintf(m_options->_fpLogFile, "\n");

    fprintf(m_options->_fpLogFile, "\nBucketRelError:\n");
    fprintf(m_options->_fpLogFile, "%lld ", (int64)m_problem->getN());
    for (auto err : _BucketError_Rel) {
      fprintf(m_options->_fpLogFile, " %.3f", err);
    }
    fprintf(m_options->_fpLogFile, "\n");
  }

  cout << endl;
  cout << "#mini-buckets:" << endl;
  cout << _MiniBuckets.size();
  for (const auto &mb : _MiniBuckets) {
    cout << " " << mb.size();
  }
  cout << endl << endl;
  cout << "Pseudowidth:" << endl;
  cout << _Pseudowidth.size();
  for (int pw : _Pseudowidth) {
    cout << " " << pw;
  }
  cout << endl << endl;
  cout << "Average relative bucket errors:" << endl;
  cout << _BucketError_Rel.size();
  for (double err : _BucketError_Rel) {
    cout << " " << err;
  }
  cout << endl;

  int32 count_zero = 0;
  int32 count_lteps = 0;
  int32 count_gteps = 0;
  for (auto q : _BucketErrorQuality) {
    if (q == 0) {
      ++count_zero;
    } else if (q == 1) {
      ++count_lteps;
    } else if (q == 2) {
      ++count_gteps;
    }
  }
  if (m_options->_fpLogFile) {
    fprintf(m_options->_fpLogFile,
            "\ncount_zero=%d, count_lteps=%d, count_gteps=%d\n", count_zero,
            count_lteps, count_gteps);
  }
  /*
  cout << m_problem->getN();
  if (m_options->aobf_subordering == "sampled_st_be") {
    for (const auto* fn : _BucketErrorFunctions) {
      //      cout << "v" << fn->getId() << ": " << endl;
      for (int j = 0; j < fn->getTableSize(); ++j) {
        cout << " " << fn->getTable()[j];
      }
    }
    cout << endl;
  }
  */

  // Try with rel or abs. absavg may make more sense here.
  if (FLAGS_aobf_subordering_use_relative_error) {
    ComputeSubtreeErrors(_BucketError_Rel);
  } else {
    ComputeSubtreeErrors(_BucketError_AbsAvg);
  }

  /*
  cout << "constants" << endl;
  for (int i = 0; i < _SubtreeError.size(); ++i) {
    cout << " " << _SubtreeError[i];
  }
  cout << endl;
    */
  if (m_options->aobf_subordering == "sampled_st_be") {
    /*
    for (const auto* fn : _BucketErrorFunctions) {
      cout << "v" << fn->getId() << ": " << endl;
      for (int j = 0; j < fn->getTableSize(); ++j) {
        cout << " " << fn->getTable()[j] << endl;
      }
      cout << endl;
    }
    */
    //    ComputeSubtreeErrorFns(_TrueSlicedBucketErrorFunctions);
    if (FLAGS_aobf_subordering_depth_limit < 0) {
      ComputeSubtreeErrorFns(_BucketErrorFunctions);
    } else {
      ComputeDepthLimitedSubtreeErrorFns(_BucketErrorFunctions,
          FLAGS_aobf_subordering_depth_limit);
    }
    /*
    cout << "fns" << endl;
    for (const auto* fn : _SubtreeErrorFunctions) {
//      cout << "v" << fn->getId() << ": " << endl;
      for (int j = 0; j < fn->getTableSize(); ++j) {
        cout << " " << fn->getTable()[j];
      }
    }
    cout << endl;
    */
  }

  //(TODO): this is for when we want to remove lookahead from experiments
  // concerning AOBF using local errors.
  // m_options->lookaheadDepth = 0;
  return 0;
}

void MiniBucketElimLH::ComputeSubtreeErrors(
    const std::vector<double> &bucket_error) {
  _SubtreeError.resize(m_problem->getN(), 0.0);
  for (int v : m_pseudotree->getElimOrder()) {
    double e = max(0.0, bucket_error[v]);
    int n_children = m_pseudotree->getNode(v)->getChildren().size();
    _SubtreeError[v] += e;  // / (n_children + 1);
    if (v != m_pseudotree->getRoot()->getVar()) {
      PseudotreeNode *p = m_pseudotree->getNode(v)->getParent();
      int p_var = p->getVar();
      int n_p_var_children = p->getChildren().size();
      int child_penalty = 1;
      int k = m_problem->getDomainSize(v);
      child_penalty = k;
      /*
      if (FLAGS_aobf_subordering_use_relative_error) {
        child_penalty += n_p_var_children;
      }
      */
      _SubtreeError[p_var] += _SubtreeError[v] / child_penalty;
    }
  }
}

// For all leaves, the subtree error function is identical
// Otherwise, add the children values into the parents.
void MiniBucketElimLH::ComputeSubtreeErrorFns(
    const std::vector<Function *> &bucket_error_functions) {
  _SubtreeErrorFunctions.resize(m_problem->getN());
  for (int i = 0; i < _SubtreeErrorFunctions.size(); ++i) {
    const auto* current_fn = bucket_error_functions[i];
    int64 table_size = current_fn->getTableSize();
    double* new_table = new double[table_size];
    for (int j = 0; j < table_size; ++j) {
      new_table[j] = 0.0;
    }
    _SubtreeErrorFunctions[i] = new FunctionBayes(
        -i, m_problem, current_fn->getScopeSet(), new_table, table_size);
  }
  for (int v : m_pseudotree->getElimOrder()) {
    const auto* current_fn = _SubtreeErrorFunctions[v];
    int n_children = m_pseudotree->getNode(v)->getChildren().size();
    if (!current_fn) {
      continue;
    }
    int64 table_size = current_fn->getTableSize();
    double* table = current_fn->getTable();
    double* be_table = bucket_error_functions[v]->getTable();
    for (int64 j = 0; j < table_size; ++j) {
      table[j] += be_table[j] / (n_children + 1);
    }

    if (v != m_pseudotree->getRoot()->getVar()) {
      PseudotreeNode *p = m_pseudotree->getNode(v)->getParent();
      int p_var = p->getVar();
      Function* parent_fn = _SubtreeErrorFunctions[p_var];

      const set<int> &var_scope = current_fn->getScopeSet();
      const set<int> &p_var_scope = parent_fn->getScopeSet();

      vector<int> var_domains;
      for (int vs : var_scope) {
        var_domains.push_back(m_problem->getDomainSize(vs));
      }

      set<int> total_scope(var_scope);
      total_scope.insert(p_var_scope.begin(), p_var_scope.end());
      set<int> intersecting_scope = intersection(var_scope, p_var_scope);
      int n = total_scope.size();
      vector<int> total_scope_vec(total_scope.begin(), total_scope.end());
      val_t *tuple = new val_t[n];
      for (int k = 0; k < n; ++k) {
        tuple[k] = 0;
      }

      set<int> v_only_scope = setminus(var_scope, p_var_scope);
      cout << "var:" << v << " | " << var_scope << "-" << p_var_scope << "="
           << v_only_scope << endl;

      // These are used to actually get values from the functions
      vector<val_t*> idx_map_v_var;
      vector<val_t*> idx_map_p_var;

      // These are used to iterate over the combination in the correct manner
      vector<val_t*> idx_map_intersect;
      vector<val_t*> idx_map_v_only_var;
      vector<val_t*> idx_map_p_only_var;

      vector<val_t> intersecting_domains;
      vector<val_t> v_only_var_domains;
      vector<val_t> p_only_var_domains;
      int64 v_only_cardinality = 1;
      int64 p_only_cardinality = 1;
      int64 intersecting_cardinality = 1;

      for (int k = 0; k < n; ++k) {
        int vs = total_scope_vec[k];
        if (ContainsKey(intersecting_scope, vs)) {
          int d = m_problem->getDomainSize(vs);
          intersecting_domains.push_back(d);
          intersecting_cardinality *= d;
          idx_map_v_var.push_back(&tuple[k]);
          idx_map_p_var.push_back(&tuple[k]);
          idx_map_intersect.push_back(&tuple[k]);
        } else if (ContainsKey(var_scope, vs)) {
          int d = m_problem->getDomainSize(vs);
          v_only_var_domains.push_back(d);
          v_only_cardinality *= d;
          idx_map_v_var.push_back(&tuple[k]);
          idx_map_v_only_var.push_back(&tuple[k]);
        } else if (ContainsKey(p_var_scope, vs)) {
          int d = m_problem->getDomainSize(vs);
          p_only_var_domains.push_back(d);
          p_only_cardinality *= d;
          idx_map_p_var.push_back(&tuple[k]);
          idx_map_p_only_var.push_back(&tuple[k]);
        }
      }

      // Iterate over the intersection and take the average over
      // the variables not in the target parent
      // For each assignment to a variable in the target parent not
      // in the current, use the averaged value for all of them.
      bool increment_done = false;
      for (int64 j = 0; j < intersecting_cardinality; ++j) {
        double value = 0.0;
        bool v_only_increment_done = false;
        for (int64 jj = 0; jj < v_only_cardinality; ++jj) {
          value += current_fn->getValuePtr(idx_map_v_var);
          v_only_increment_done =
              !IdxMapIncrement(idx_map_v_only_var, v_only_var_domains);
        }
        assert(v_only_increment_done);
        value /= v_only_cardinality;

        bool p_only_increment_done = false;
        int n_p_var_children = p->getChildren().size();
        int child_penalty = m_problem->getDomainSize(p_var);

        for (int64 jj = 0; jj < p_only_cardinality; ++jj) {
          double p_value = parent_fn->getValuePtr(idx_map_p_var);
          parent_fn->setValuePtr(idx_map_p_var,
                                 p_value + value / child_penalty);

          p_only_increment_done =
              !IdxMapIncrement(idx_map_p_only_var, p_only_var_domains);
        }
        assert(p_only_increment_done);

        increment_done =
            !IdxMapIncrement(idx_map_intersect, intersecting_domains);
      }
      assert(increment_done);
    }
  }
}

void MiniBucketElimLH::ComputeDepthLimitedSubtreeErrorFns(
    const std::vector<Function*>& bucket_error_fns, int depth_limit) {
  // temporary functions representing "buckets"
  _SubtreeErrorFunctions.resize(m_problem->getN(), nullptr);
  vector<Function*> buckets(_SubtreeErrorFunctions.size(), nullptr);

  // Initialize subtree error functions and buckets
  for (int i = 0; i < _SubtreeErrorFunctions.size(); ++i) {
    const auto* current_fn = bucket_error_fns[i];
    int64 table_size = current_fn->getTableSize();
    double* new_table = new double[table_size]();
    _SubtreeErrorFunctions[i] = new FunctionBayes(
        -i, m_problem, current_fn->getScopeSet(), new_table, table_size);
    new_table = new double[table_size]();
    buckets[i] = new FunctionBayes(
        -i, m_problem, current_fn->getScopeSet(), new_table, table_size);
  }

  for (int var : m_pseudotree->getElimOrder()) {
    // Generate the list of variables within depth d of v
    stack<int> processing_stack;
    
    // stack contains a var, depth (relative to v) pair.
    stack<pair<int,int>> dfs;
    dfs.push(make_pair(var, 0));

    // remember the buffer bucket and replace the root with the one that will
    // become the final result
    Function* buffer_bucket = buckets[var];
    buckets[var] = _SubtreeErrorFunctions[var];
    while (!dfs.empty()) {
      int c_var;
      int c_depth;
      tie(c_var, c_depth) = dfs.top();
      dfs.pop(); 

      // set variable's bucket function to the error function
      for (int k = 0; k < buckets[c_var]->getTableSize(); ++k) {
        buckets[c_var]->getTable()[k] = bucket_error_fns[c_var]->getTable()[k];
      }

      // skip root -- it does not need to send messages
      if (c_var != var) {
        processing_stack.push(c_var);
      }
      
      if (c_depth < depth_limit) {
        const PseudotreeNode* node = m_pseudotree->getNode(c_var);
        for (const PseudotreeNode* c : node->getChildren()) {
          dfs.push(make_pair(c->getVar(), c_depth + 1));
        }
      }
    }

    // Process each variable bottom up.
    while (!processing_stack.empty()) {
      int v = processing_stack.top();
      processing_stack.pop();
      int p = m_pseudotree->getNode(v)->getParent()->getVar();
      Function* v_fn = buckets[v];
      Function* p_fn = buckets[p];

      // send message v->p
      // these messages are different in that variable v has already been
      // eliminated.
      // the process is as follows:
      // project out the variables not in the parent scope (avg, min, max)
      // duplicate the message over cardinality of the parent scope variables
      // not in the message
      const set<int> &v_scope = v_fn->getScopeSet();
      const set<int> &p_scope = p_fn->getScopeSet();

      set<int> agg_scope = setminus(v_scope, p_scope);
      set<int> dupe_scope = setminus(p_scope, v_scope);
      set<int> both_scope = intersection(v_scope, p_scope);
      vector<val_t> agg_domains;
      int64 agg_card = 1;
      for (int vv : agg_scope) {
        agg_domains.push_back(m_problem->getDomainSize(vv));
        agg_card *= m_problem->getDomainSize(vv);
      }
      vector<val_t> dupe_domains;
      for (int vv : dupe_scope) {
        dupe_domains.push_back(m_problem->getDomainSize(vv));
      }
      vector<val_t> both_domains;
      for (int vv : both_scope) {
        both_domains.push_back(m_problem->getDomainSize(vv));
      }

      set<int> all_vars(v_scope);
      all_vars.insert(p_scope.begin(), p_scope.end());

      int n = all_vars.size();
      vector<int> all_vars_vec(all_vars.begin(), all_vars.end());
      val_t *tuple = new val_t[n];
      for (int k = 0; k < n; ++k) {
        tuple[k] = 0;
      }

      vector<val_t*> idx_map_p;
      vector<val_t*> idx_map_v;
      vector<val_t*> idx_map_agg;
      vector<val_t*> idx_map_dupe;
      vector<val_t*> idx_map_both;

      for (int k = 0; k < n; ++k) {
        int vs = all_vars_vec[k];
        if (ContainsKey(both_scope, vs)) {
          idx_map_v.push_back(&tuple[k]);
          idx_map_p.push_back(&tuple[k]);
          idx_map_both.push_back(&tuple[k]);
        } else if (ContainsKey(v_scope, vs)) {
          idx_map_v.push_back(&tuple[k]);
          idx_map_agg.push_back(&tuple[k]);
        } else if (ContainsKey(p_scope, vs)) {
          idx_map_p.push_back(&tuple[k]);
          idx_map_dupe.push_back(&tuple[k]);
        }
      }

      // Iterate over intersection
      do {
        // TODO(lamw): should consider other aggregation functions
        //
        // AVERAGE
        /*
        double value = 0.0;
        do {
          value += v_fn->getValuePtr(idx_map_v);
        } while (IdxMapIncrement(idx_map_agg, agg_domains));
        value /= agg_card;
        */
        // MIN
        double value = numeric_limits<double>::infinity();
        do {
          value = min(value, v_fn->getValuePtr(idx_map_v));
        } while (IdxMapIncrement(idx_map_agg, agg_domains));
        // MAX
        /*
        double value = -numeric_limits<double>::infinity();
        do {
          value = max(value, v_fn->getValuePtr(idx_map_v));
        } while (IdxMapIncrement(idx_map_agg, agg_domains));
        */

        // duplicate value across compatible instantiations of parent
        do {
          double p_value = p_fn->getValuePtr(idx_map_p);
          p_fn->setValuePtr(idx_map_p, p_value + value);
        } while (IdxMapIncrement(idx_map_dupe, dupe_domains));
      } while (IdxMapIncrement(idx_map_both, both_domains));
    }
    // restore buffer bucket
    buckets[var] = buffer_bucket;
  }
}

}  // namespace daoopt
