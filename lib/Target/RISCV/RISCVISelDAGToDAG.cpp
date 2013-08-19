//===-- RISCVISelDAGToDAG.cpp - A dag to dag inst selector for RISCV --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the RISCV target.
//
//===----------------------------------------------------------------------===//

#include "RISCVTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
// Used to build addressing modes.
struct RISCVAddressingMode {
  // The shape of the address.
  enum AddrForm {
    // base+offset
    FormBO
  };
  AddrForm Form;

  // The type of displacement. 
  enum OffRange {
    Off12Only
  };
  OffRange OffR;

  // The parts of the address.  The address is equivalent to:
  //
  //     Base + Offset + Index + (IncludesDynAlloc ? ADJDYNALLOC : 0)
  SDValue Base;
  int64_t Offset;

  RISCVAddressingMode(AddrForm form, OffRange offr)
    : Form(form), OffR(offr), Base(), Offset(0) {}

  void dump() {
    errs() << "RISCVAddressingMode " << this << '\n';

    errs() << " Base ";
    if (Base.getNode() != 0)
      Base.getNode()->dump();
    else
      errs() << "null\n";

    errs() << " Offset " << Offset;
  }
};

class RISCVDAGToDAGISel : public SelectionDAGISel {
  const RISCVTargetLowering &Lowering;
  const RISCVSubtarget &Subtarget;

  // Used by RISCVOperands.td to create integer constants.
  inline SDValue getImm(const SDNode *Node, uint64_t Imm) {
    return CurDAG->getTargetConstant(Imm, Node->getValueType(0));
  }

  // Try to fold more of the base or index of AM into AM, where IsBase
  // selects between the base and index.
  bool expandAddress(RISCVAddressingMode &AM, bool IsBase);

  // Try to describe N in AM, returning true on success.
  bool selectAddress(SDValue N, RISCVAddressingMode &AM);

  // Extract individual target operands from matched address AM.
  void getAddressOperands(const RISCVAddressingMode &AM, EVT VT,
                          SDValue &Base, SDValue &Disp);
  void getAddressOperands(const RISCVAddressingMode &AM, EVT VT,
                          SDValue &Base, SDValue &Disp, SDValue &Index);

  //RISCV
  bool selectMemRegAddr(SDValue Addr, SDValue &Base, SDValue &Offset) {
      
    EVT ValTy = Addr.getValueType();

    // if Address is FI, get the TargetFrameIndex.
    if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
      Base   = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
      Offset = CurDAG->getTargetConstant(0, ValTy);
      return true;
    }

    if (TM.getRelocationModel() != Reloc::PIC_) {
      if ((Addr.getOpcode() == ISD::TargetExternalSymbol ||
          Addr.getOpcode() == ISD::TargetGlobalAddress))
        return false;
    }

    // Addresses of the form FI+const or FI|const
    if (CurDAG->isBaseWithConstantOffset(Addr)) {
      ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1));
      if (isInt<16>(CN->getSExtValue())) {
  
        // If the first operand is a FI, get the TargetFI Node
        if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>
                                    (Addr.getOperand(0)))
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
        else
          Base = Addr.getOperand(0);
  
        Offset = CurDAG->getTargetConstant(CN->getZExtValue(), ValTy);
        return true;
      }
    }

    //Last case
    Base = Addr;
    Offset = CurDAG->getTargetConstant(0, Addr.getValueType());
    return true;
  }
  //End RISCV

  // PC-relative address matching routines used by RISCVOperands.td.
  bool selectPCRelAddress(SDValue Addr, SDValue &Target) {
    if (Addr.getOpcode() == RISCVISD::PCREL_WRAPPER) {
      Target = Addr.getOperand(0);
      return true;
    }
    return false;
  }

  // If Op0 is null, then Node is a constant that can be loaded using:
  //
  //   (Opcode UpperVal LowerVal)
  //
  // If Op0 is nonnull, then Node can be implemented using:
  //
  //   (Opcode (Opcode Op0 UpperVal) LowerVal)
  SDNode *splitLargeImmediate(unsigned Opcode, SDNode *Node, SDValue Op0,
                              uint64_t UpperVal, uint64_t LowerVal);

public:
  RISCVDAGToDAGISel(RISCVTargetMachine &TM, CodeGenOpt::Level OptLevel)
    : SelectionDAGISel(TM, OptLevel),
      Lowering(*TM.getTargetLowering()),
      Subtarget(*TM.getSubtargetImpl()) { }

  // Override MachineFunctionPass.
  virtual const char *getPassName() const LLVM_OVERRIDE {
    return "RISCV DAG->DAG Pattern Instruction Selection";
  }

  // Override SelectionDAGISel.
  virtual SDNode *Select(SDNode *Node) LLVM_OVERRIDE;
  virtual bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                            char ConstraintCode,
                                            std::vector<SDValue> &OutOps)
    LLVM_OVERRIDE;

  // Include the pieces autogenerated from the target description.
  #include "RISCVGenDAGISel.inc"
};
} // end anonymous namespace

FunctionPass *llvm::createRISCVISelDag(RISCVTargetMachine &TM,
                                         CodeGenOpt::Level OptLevel) {
  return new RISCVDAGToDAGISel(TM, OptLevel);
}

// Return true if Val should be selected as a displacement for an address
// with range DR.  Here we're interested in the range of both the instruction
// described by DR and of any pairing instruction.
static bool selectOffset(RISCVAddressingMode::OffRange OffR, int64_t Val) {
  switch (OffR) {
  case RISCVAddressingMode::Off12Only:
    return isUInt<12>(Val);
  }
  llvm_unreachable("Unhandled offset range");
}

// The base or index of AM is equivalent to Op0 + Op1, where IsBase selects
// between the base and index.  Try to fold Op1 into AM's displacement.
static bool expandOffset(RISCVAddressingMode &AM, bool IsBase,
                       SDValue Op0, ConstantSDNode *Op1) {
  // First try adjusting the displacement.
  int64_t TestOffset = AM.Offset + Op1->getSExtValue();
  if (selectOffset(AM.OffR, TestOffset)) {
    //changeComponent(AM, IsBase, Op0);
    AM.Base = Op0;
    AM.Offset = TestOffset;
    return true;
  }

  // We could consider forcing the displacement into a register and
  // using it as an index, but it would need to be carefully tuned.
  return false;
}

bool RISCVDAGToDAGISel::expandAddress(RISCVAddressingMode &AM,
                                        bool IsBase) {
  //SDValue N = IsBase ? AM.Base : AM.Index;
  SDValue N = AM.Base;
  unsigned Opcode = N.getOpcode();
  if (Opcode == ISD::TRUNCATE) {
    N = N.getOperand(0);
    Opcode = N.getOpcode();
  }
  if (Opcode == ISD::ADD || CurDAG->isBaseWithConstantOffset(N)) {
    SDValue Op0 = N.getOperand(0);
    SDValue Op1 = N.getOperand(1);

    unsigned Op0Code = Op0->getOpcode();
    unsigned Op1Code = Op1->getOpcode();

    if (Op0Code == ISD::Constant)
      return expandOffset(AM, IsBase, Op1, cast<ConstantSDNode>(Op0));
    if (Op1Code == ISD::Constant)
      return expandOffset(AM, IsBase, Op0, cast<ConstantSDNode>(Op1));

  }
  return false;
}

// Return true if an instruction with displacement range DR should be
// used for displacement value Val.  selectDisp(DR, Val) must already hold.
static bool isValidOffset(RISCVAddressingMode::OffRange OffR, int64_t Val) {
  assert(selectOffset(OffR, Val) && "Invalid displacement");
  switch (OffR) {
  case RISCVAddressingMode::Off12Only:
    return true;
  }
  llvm_unreachable("Unhandled displacement range");
}

// Return true if Base + Disp + Index should be performed by LA(Y).
static bool shouldUseLA(SDNode *Base, int64_t Disp, SDNode *Index) {
  // Don't use LA(Y) for constants.
  if (!Base)
    return false;

  // Always use LA(Y) for frame addresses, since we know that the destination
  // register is almost always (perhaps always) going to be different from
  // the frame register.
  if (Base->getOpcode() == ISD::FrameIndex)
    return true;

  if (Disp) {
    // Always use LA(Y) if there is a base, displacement and index.
    if (Index)
      return true;

    // Always use LA if the displacement is small enough.  It should always
    // be no worse than AGHI (and better if it avoids a move).
    if (isUInt<12>(Disp))
      return true;

    // For similar reasons, always use LAY if the constant is too big for AGHI.
    // LAY should be no worse than AGFI.
    if (!isInt<16>(Disp))
      return true;
  } else {
    // Don't use LA for plain registers.
    if (!Index)
      return false;

    // Don't use LA for plain addition if the index operand is only used
    // once.  It should be a natural two-operand addition in that case.
    if (Index->hasOneUse())
      return false;

    // Prefer addition if the second operation is sign-extended, in the
    // hope of using AGF.
    unsigned IndexOpcode = Index->getOpcode();
    if (IndexOpcode == ISD::SIGN_EXTEND ||
        IndexOpcode == ISD::SIGN_EXTEND_INREG)
      return false;
  }

  // Don't use LA for two-operand addition if either operand is only
  // used once.  The addition instructions are better in that case.
  if (Base->hasOneUse())
    return false;

  return true;
}

// Return true if Addr is suitable for AM, updating AM if so.
bool RISCVDAGToDAGISel::selectAddress(SDValue Addr,
                                        RISCVAddressingMode &AM) {
  // Start out assuming that the address will need to be loaded separately,
  // then try to extend it as much as we can.
  AM.Base = Addr;

  // First try treating the address as a constant.
  if (Addr.getOpcode() == ISD::Constant &&
      expandOffset(AM, true, SDValue(), cast<ConstantSDNode>(Addr)))
    ;

  // Reject cases where the other instruction in a pair should be used.
  if (!isValidOffset(AM.OffR, AM.Offset))
    return false;

  DEBUG(AM.dump());
  return true;
}

// Insert a node into the DAG at least before Pos.  This will reposition
// the node as needed, and will assign it a node ID that is <= Pos's ID.
// Note that this does *not* preserve the uniqueness of node IDs!
// The selection DAG must no longer depend on their uniqueness when this
// function is used.
static void insertDAGNode(SelectionDAG *DAG, SDNode *Pos, SDValue N) {
  if (N.getNode()->getNodeId() == -1 ||
      N.getNode()->getNodeId() > Pos->getNodeId()) {
    DAG->RepositionNode(Pos, N.getNode());
    N.getNode()->setNodeId(Pos->getNodeId());
  }
}

void RISCVDAGToDAGISel::getAddressOperands(const RISCVAddressingMode &AM,
                                             EVT VT, SDValue &Base,
                                             SDValue &Offset) {
  Base = AM.Base;
  if (!Base.getNode())
    // Register 0 means "no base".  This is mostly useful for shifts.
    Base = CurDAG->getRegister(0, VT);
  else if (Base.getOpcode() == ISD::FrameIndex) {
    // Lower a FrameIndex to a TargetFrameIndex.
    int64_t FrameIndex = cast<FrameIndexSDNode>(Base)->getIndex();
    Base = CurDAG->getTargetFrameIndex(FrameIndex, VT);
  } else if (Base.getValueType() != VT) {
    // Truncate values from i64 to i32, for shifts.
    assert(VT == MVT::i32 && Base.getValueType() == MVT::i64 &&
           "Unexpected truncation");
    DebugLoc DL = Base.getDebugLoc();
    SDValue Trunc = CurDAG->getNode(ISD::TRUNCATE, DL, VT, Base);
    insertDAGNode(CurDAG, Base.getNode(), Trunc);
    Base = Trunc;
  }

  // Lower the displacement to a TargetConstant.
  Offset = CurDAG->getTargetConstant(AM.Offset, VT);
}


SDNode *RISCVDAGToDAGISel::splitLargeImmediate(unsigned Opcode, SDNode *Node,
                                                 SDValue Op0, uint64_t UpperVal,
                                                 uint64_t LowerVal) {
  EVT VT = Node->getValueType(0);
  DebugLoc DL = Node->getDebugLoc();
  SDValue Upper = CurDAG->getConstant(UpperVal, VT);
  if (Op0.getNode())
    Upper = CurDAG->getNode(Opcode, DL, VT, Op0, Upper);
  Upper = SDValue(Select(Upper.getNode()), 0);

  SDValue Lower = CurDAG->getConstant(LowerVal, VT);
  SDValue Or = CurDAG->getNode(Opcode, DL, VT, Upper, Lower);
  return Or.getNode();
}

SDNode *RISCVDAGToDAGISel::Select(SDNode *Node) {
  // Dump information about the Node being selected
  DEBUG(errs() << "Selecting: "; Node->dump(CurDAG); errs() << "\n");

  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    DEBUG(errs() << "== "; Node->dump(CurDAG); errs() << "\n");
    return 0;
  }

  unsigned Opcode = Node->getOpcode();
  //TODO: Any special selections here?
  //would be based on a switch of opcodes to ISD:: nodes

  // Select the default instruction
  SDNode *ResNode = SelectCode(Node);

  DEBUG(errs() << "=> ";
        if (ResNode == NULL || ResNode == Node)
          Node->dump(CurDAG);
        else
          ResNode->dump(CurDAG);
        errs() << "\n";
        );
  return ResNode;
}

bool RISCVDAGToDAGISel::
SelectInlineAsmMemoryOperand(const SDValue &Op,
                             char ConstraintCode,
                             std::vector<SDValue> &OutOps) {
  assert(ConstraintCode == 'm' && "Unexpected constraint code");
  // Accept addresses with short displacements, which are compatible
  // with Q, R, S and T.  But keep the index operand for future expansion.
  SDValue Base, Disp, Index;

  OutOps.push_back(Base);
  OutOps.push_back(Disp);
  OutOps.push_back(Index);
  return false;
}
