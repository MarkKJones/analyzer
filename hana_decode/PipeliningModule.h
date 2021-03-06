#ifndef Podd_PipeliningModule_h_
#define Podd_PipeliningModule_h_

/////////////////////////////////////////////////////////////////////
//
//   PipeliningModule
//   R. Michaels, Oct 2016
//
//   This class splits the "CODA event buffer" into actual events so that the
//   Podd analyzer's event loop works as before.
//   An "event" is a particle hitting a target and making hits in detectors, etc.
//   A "CODA event buffer" can contain many "events"; the events are stored in
//   each module of this type (piplelining).
//
//   All of the Jlab pipeline modules have the same data format with respect to
//   specific bits indicating data types.
//   All produce block headers, block trailers, and event headers.
//   All encode the slot number the same way in these headers and trailers.
//
//   While we're at it, we also split by slot number
//   so that each module doesn't need to consider another module's data.
//   Note, one module belongs to one slot.
//
//   The first event buffer will have the block header
//   the last event buffer will have the block trailer
//   and all event buffers will have an event header
//
/////////////////////////////////////////////////////////////////////

#include "VmeModule.h"
#include <iostream>

namespace Decoder {

class PipeliningModule : public VmeModule  {

public:

   PipeliningModule()
    : fNWarnings(0), fBlockHeader(0), data_type_def(15), fFirstTime(true),
      index_buffer(0) {}
   PipeliningModule( UInt_t crate, UInt_t slot );
   PipeliningModule(const PipeliningModule &fh) = delete;
   PipeliningModule& operator=(const PipeliningModule &fh) = delete;
   virtual ~PipeliningModule() = default;

   void PrintBlocks();

protected:

   virtual Int_t SplitBuffer( const std::vector<UInt_t>& bigbuffer );
   void ReStart();
   std::vector< UInt_t >GetNextBlock();
   virtual UInt_t LoadNextEvBuffer( THaSlotData *sldat )=0;
   virtual UInt_t LoadThisBlock( THaSlotData *sldat, const std::vector<UInt_t>& evb ) = 0;
   UInt_t fNWarnings;
   UInt_t fBlockHeader;
   UInt_t data_type_def;

   Bool_t fFirstTime;

   std::vector<std::vector<UInt_t>> eventblock;
   UInt_t index_buffer;
   UInt_t GetIndex();

   ClassDef(Decoder::PipeliningModule,0)  // A pipelining module

};

}

#endif
