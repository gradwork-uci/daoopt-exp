/*
 * DaooptInterface.h
 *
 *  Created on: May 24, 2014
 *      Author: lars
 */

#ifndef DAOOPTINTERFACE_H_
#define DAOOPTINTERFACE_H_

#include "Main.h"


namespace daoopt {

class DaooptInterface {

protected:
  bool m_initialized;
  bool m_preprocessed;
  scoped_ptr<Main> m_main;

public:
  DaooptInterface();

  // Combine Initalization + SLS stop before compiling heuristics
  // from preprocessing ~ initDataStructs
  bool preprocessAndSLS(int argc, char** argv);
  bool preprocessAndSLS(const ProgramOptions& options);
  bool preprocessNoSLS(const ProgramOptions& options, int & ErrorCode);

  // Divide Compile Heuristic step into 3 separte steps, FGLP, JGLP, MBEMM
  // MBEMM is the last step,so afer compiling MBEMM exec runLDS(), finishPreproc()
  bool compileFGLP(int mplp, int mplps);
  bool compileJGLP(int jglp, int jglps, int ibound);
  bool compileMbeAndFinishPreprocessing();

  // Initialize the computation. Returns true on success.
  bool initialize();

  // Perform preprocessing, based on arguments passed in like command line
  // flags (["-i", "10", "--sls", ...]). Returns true on success.
  bool preprocess(int argc, char** argv);

  // Perform preprocessing, based on options set in ProgramOptions object
  // instance. Returns true on success.
  bool preprocess(const ProgramOptions& options);

  // Estimate the complexity of the problem (in log10 number of nodes expanded)
  // based on a search sample of the given size. Returns 0 if the problem was
  // solved completely during estimation.
  double estimate(size_t sampleSize = 20000) const;

  // Solve the problem. Only do <limit> node expansions at a time (0=unlimited),
  // returns true if solved to completion, false otherwise.
  // To emulate anytime behavior, run with nodeLimit 10k or so and interleave
  // with getSolution below.
  bool solve(size_t nodeLimit = 0);

  // run SLS (e.g. GLS+). #iterations and time per iteration are given.
  // returns 0 iff ok.
  int runSLS(int nSlsIter, int slsTimePerIter) ;
  int stopSLS(void) ;
  bool execSLS(int nSlsIter, int slsTimePerIter);

  // Gets the current best known solution. Also stores the corresponding
  // assignment in the referenced vector (if != null).
  double getSolution(vector<int>* assignment = NULL) const;

  bool exportProblemStats(int& fN, int& fF, int& fK, int& fS, int& fW, int& fH);

  int outputStatistics(void) ;
  int64 getNumORnodesExpanded(void) ;

  Heuristic *getHeuristic(void) { return m_main.get()->getHeuristic() ; }
private:
  bool preprocessCommon();
};

}  // namespace daoopt

#endif /* DAOOPTINTERFACE_H_ */
