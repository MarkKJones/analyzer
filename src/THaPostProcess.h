#ifndef HALLA_THaPostProcess
#define HALLA_THaPostProcess

#include "TObject.h"

class THaRun;
class THaEvData;
class TDatime;
class TList;

class THaPostProcess : public TObject {
 public:
  THaPostProcess();
  virtual ~THaPostProcess();
  virtual Int_t Init(const TDatime& )=0;
  virtual Int_t Process( const THaEvData*, const THaRun*, Int_t code )=0;
  virtual Int_t Close()=0;
 protected:
  Int_t fIsInit;

  static TList* fgModules; // List of all current PostProcess modules

  ClassDef(THaPostProcess,0)
};

#endif
