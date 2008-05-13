//===-- PIC16ISelDAGToDAG.cpp - A dag to dag inst selector for PIC16 ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source 
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the PIC16 target.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "pic16-isel"

#include "PIC16.h"
#include "PIC16ISelLowering.h"
#include "PIC16RegisterInfo.h"
#include "PIC16Subtarget.h"
#include "PIC16TargetMachine.h"
#include "llvm/GlobalValue.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Type.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetMachine.h"
#include <queue>
#include <set>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// PIC16DAGToDAGISel - PIC16 specific code to select PIC16 machine
// instructions for SelectionDAG operations.
//===----------------------------------------------------------------------===//
namespace {

class VISIBILITY_HIDDEN PIC16DAGToDAGISel : public SelectionDAGISel {

  /// TM - Keep a reference to PIC16TargetMachine.
  PIC16TargetMachine &TM;

  /// PIC16Lowering - This object fully describes how to lower LLVM code to an
  /// PIC16-specific SelectionDAG.
  PIC16TargetLowering PIC16Lowering;

  /// Subtarget - Keep a pointer to the PIC16Subtarget around so that we can
  /// make the right decision when generating code for different targets.
  //TODO: add initialization on constructor
  //const PIC16Subtarget *Subtarget;
 
public:
  PIC16DAGToDAGISel(PIC16TargetMachine &tm) : 
        SelectionDAGISel(PIC16Lowering),
        TM(tm), PIC16Lowering(*TM.getTargetLowering()) {}
  
  virtual void InstructionSelectBasicBlock(SelectionDAG &SD);

  // Pass Name
  virtual const char *getPassName() const {
    return "PIC16 DAG->DAG Pattern Instruction Selection";
  } 
  
private:
  // Include the pieces autogenerated from the target description.
  #include "PIC16GenDAGISel.inc"

  SDNode *Select(SDOperand N);

  // Select addressing mode. currently assume base + offset addr mode.
  bool SelectAM(SDOperand Op, SDOperand N, SDOperand &Base, SDOperand &Offset);
  bool SelectDirectAM(SDOperand Op, SDOperand N, SDOperand &Base, 
		      SDOperand &Offset);
  bool StoreInDirectAM(SDOperand Op, SDOperand N, SDOperand &fsr);
  bool LoadFSR(SDOperand Op, SDOperand N, SDOperand &Base, SDOperand &Offset);
  bool LoadNothing(SDOperand Op, SDOperand N, SDOperand &Base, 
		   SDOperand &Offset);

  // getI8Imm - Return a target constant with the specified
  // value, of type i8.
  inline SDOperand getI8Imm(unsigned Imm) {
    return CurDAG->getTargetConstant(Imm, MVT::i8);
  }


  #ifndef NDEBUG
  unsigned Indent;
  #endif
};

}

/// InstructionSelectBasicBlock - This callback is invoked by
/// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
void PIC16DAGToDAGISel::InstructionSelectBasicBlock(SelectionDAG &SD) 
{
  DEBUG(BB->dump());
  // Codegen the basic block.
  #ifndef NDEBUG
  DOUT << "===== Instruction selection begins:\n";
  Indent = 0;
  #endif

  // Select target instructions for the DAG.
  SD.setRoot(SelectRoot(SD.getRoot()));

  #ifndef NDEBUG
  DOUT << "===== Instruction selection ends:\n";
  #endif

  SD.RemoveDeadNodes();
  
  // Emit machine code to BB. 
  ScheduleAndEmitDAG(SD);
}


bool PIC16DAGToDAGISel::
SelectDirectAM (SDOperand Op, SDOperand N, SDOperand &Base, SDOperand &Offset)
{
  GlobalAddressSDNode *GA;
  ConstantSDNode      *GC;

  // if Address is FI, get the TargetFrameIndex.
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(N)) {
    cout << "--------- its frame Index\n";
    Base   = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
    Offset = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  if (N.getOpcode() == ISD::GlobalAddress) {
    GA = dyn_cast<GlobalAddressSDNode>(N);
    Offset = CurDAG->getTargetConstant((unsigned char)GA->getOffset(), MVT::i8);
    Base = CurDAG->getTargetGlobalAddress(GA->getGlobal(), MVT::i16,
                                          GA->getOffset());
    return true;
  } 

  if (N.getOpcode() == ISD::ADD) {
    GC = dyn_cast<ConstantSDNode>(N.getOperand(1));
    Offset = CurDAG->getTargetConstant((unsigned char)GC->getValue(), MVT::i8);
    if ((GA = dyn_cast<GlobalAddressSDNode>(N.getOperand(0)))) {
      Base = CurDAG->getTargetGlobalAddress(GA->getGlobal(), MVT::i16, 
	        		            GC->getValue());
      return true;
    }
    else if (FrameIndexSDNode *FIN 
		= dyn_cast<FrameIndexSDNode>(N.getOperand(0))) {
      Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
      return true;
    }
  }

  return false;  
}


//FIXME: must also account for preinc/predec/postinc/postdec
bool PIC16DAGToDAGISel::
StoreInDirectAM (SDOperand Op, SDOperand N, SDOperand &fsr)
{
  RegisterSDNode *Reg;
  if (N.getOpcode() == ISD::LOAD) {
    LoadSDNode *LD = dyn_cast<LoadSDNode>(N);
    if (LD) {
      fsr = LD->getBasePtr();
    }
    else if (isa<RegisterSDNode>(N.Val)) { 
      //FIXME an attempt to retrieve the register number
      //but does not work
      cout << "this is a register\n";
      Reg = dyn_cast<RegisterSDNode>(N.Val);
      fsr = CurDAG->getRegister(Reg->getReg(),MVT::i16);  
    }
    else {
      cout << "this is not a register\n";
      // FIXME must use whatever load is using
      fsr = CurDAG->getRegister(1,MVT::i16);
    }
    return true;
  }
  return false;  
}

bool PIC16DAGToDAGISel::
LoadFSR (SDOperand Op, SDOperand N, SDOperand &Base, SDOperand &Offset)
{
  GlobalAddressSDNode *GA;

  if (N.getOpcode() == ISD::GlobalAddress) {
    GA = dyn_cast<GlobalAddressSDNode>(N);
    Offset = CurDAG->getTargetConstant((unsigned char)GA->getOffset(), MVT::i8);
    Base = CurDAG->getTargetGlobalAddress(GA->getGlobal(), MVT::i16,
					  GA->getOffset());
    return true;
  }
  else if (N.getOpcode() == PIC16ISD::Package) {
    CurDAG->setGraphColor(Op.Val, "blue");
    CurDAG->viewGraph();
  }

  return false;
}

//don't thake this seriously, it will change
bool PIC16DAGToDAGISel::
LoadNothing (SDOperand Op, SDOperand N, SDOperand &Base, SDOperand &Offset)
{
  GlobalAddressSDNode *GA;
  if (N.getOpcode() == ISD::GlobalAddress) {
    GA = dyn_cast<GlobalAddressSDNode>(N);
    cout << "==========" << GA->getOffset() << "\n";
    Offset = CurDAG->getTargetConstant((unsigned char)GA->getOffset(), MVT::i8);
    Base = CurDAG->getTargetGlobalAddress(GA->getGlobal(), MVT::i16,
					  GA->getOffset());
    return true;
  }  

  return false;
}


/// Select instructions not customized! Used for
/// expanded, promoted and normal instructions
SDNode* PIC16DAGToDAGISel::Select(SDOperand N) 
{
  SDNode *Node = N.Val;
  unsigned Opcode = Node->getOpcode();

  // Dump information about the Node being selected
  #ifndef NDEBUG
  DOUT << std::string(Indent, ' ') << "Selecting: ";
  DEBUG(Node->dump(CurDAG));
  DOUT << "\n";
  Indent += 2;
  #endif

  // If we have a custom node, we already have selected!
  if (Opcode >= ISD::BUILTIN_OP_END && Opcode < PIC16ISD::FIRST_NUMBER) {
    #ifndef NDEBUG
    DOUT << std::string(Indent-2, ' ') << "== ";
    DEBUG(Node->dump(CurDAG));
    DOUT << "\n";
    Indent -= 2;
    #endif
    return NULL;
  }

  ///
  // Instruction Selection not handled by custom or by the 
  // auto-generated tablegen selection should be handled here.
  /// 
  switch(Opcode) {
    default: break;
  }

  // Select the default instruction.
  SDNode *ResNode = SelectCode(N);

  #ifndef NDEBUG
  DOUT << std::string(Indent-2, ' ') << "=> ";
  if (ResNode == NULL || ResNode == N.Val)
    DEBUG(N.Val->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DOUT << "\n";
  Indent -= 2;
  #endif

  return ResNode;
}

/// createPIC16ISelDag - This pass converts a legalized DAG into a 
/// PIC16-specific DAG, ready for instruction scheduling.
FunctionPass *llvm::createPIC16ISelDag(PIC16TargetMachine &TM) {
  return new PIC16DAGToDAGISel(TM);
}

