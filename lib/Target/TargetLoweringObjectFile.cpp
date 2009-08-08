//===-- llvm/Target/TargetLoweringObjectFile.cpp - Object File Info -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements classes used to handle lowerings specific to common
// object file formats.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/GlobalVariable.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSection.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Mangler.h"
#include "llvm/ADT/StringExtras.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
//                              Generic Code
//===----------------------------------------------------------------------===//

TargetLoweringObjectFile::TargetLoweringObjectFile() : Ctx(0) {
  TextSection = 0;
  DataSection = 0;
  BSSSection = 0;
  ReadOnlySection = 0;
  StaticCtorSection = 0;
  StaticDtorSection = 0;
  LSDASection = 0;
  EHFrameSection = 0;

  DwarfAbbrevSection = 0;
  DwarfInfoSection = 0;
  DwarfLineSection = 0;
  DwarfFrameSection = 0;
  DwarfPubNamesSection = 0;
  DwarfPubTypesSection = 0;
  DwarfDebugInlineSection = 0;
  DwarfStrSection = 0;
  DwarfLocSection = 0;
  DwarfARangesSection = 0;
  DwarfRangesSection = 0;
  DwarfMacroInfoSection = 0;
}

TargetLoweringObjectFile::~TargetLoweringObjectFile() {
}

static bool isSuitableForBSS(const GlobalVariable *GV) {
  Constant *C = GV->getInitializer();
  
  // Must have zero initializer.
  if (!C->isNullValue())
    return false;
  
  // Leave constant zeros in readonly constant sections, so they can be shared.
  if (GV->isConstant())
    return false;
  
  // If the global has an explicit section specified, don't put it in BSS.
  if (!GV->getSection().empty())
    return false;
  
  // If -nozero-initialized-in-bss is specified, don't ever use BSS.
  if (NoZerosInBSS)
    return false;
  
  // Otherwise, put it in BSS!
  return true;
}

/// IsNullTerminatedString - Return true if the specified constant (which is
/// known to have a type that is an array of 1/2/4 byte elements) ends with a
/// nul value and contains no other nuls in it.
static bool IsNullTerminatedString(const Constant *C) {
  const ArrayType *ATy = cast<ArrayType>(C->getType());
  
  // First check: is we have constant array of i8 terminated with zero
  if (const ConstantArray *CVA = dyn_cast<ConstantArray>(C)) {
    if (ATy->getNumElements() == 0) return false;

    ConstantInt *Null =
      dyn_cast<ConstantInt>(CVA->getOperand(ATy->getNumElements()-1));
    if (Null == 0 || Null->getZExtValue() != 0)
      return false; // Not null terminated.
    
    // Verify that the null doesn't occur anywhere else in the string.
    for (unsigned i = 0, e = ATy->getNumElements()-1; i != e; ++i)
      // Reject constantexpr elements etc.
      if (!isa<ConstantInt>(CVA->getOperand(i)) ||
          CVA->getOperand(i) == Null)
        return false;
    return true;
  }

  // Another possibility: [1 x i8] zeroinitializer
  if (isa<ConstantAggregateZero>(C))
    return ATy->getNumElements() == 1;

  return false;
}

/// getKindForGlobal - This is a top-level target-independent classifier for
/// a global variable.  Given an global variable and information from TM, it
/// classifies the global in a variety of ways that make various target
/// implementations simpler.  The target implementation is free to ignore this
/// extra info of course.
SectionKind TargetLoweringObjectFile::getKindForGlobal(const GlobalValue *GV,
                                                       const TargetMachine &TM){
  assert(!GV->isDeclaration() && !GV->hasAvailableExternallyLinkage() &&
         "Can only be used for global definitions");
  
  Reloc::Model ReloModel = TM.getRelocationModel();
  
  // Early exit - functions should be always in text sections.
  const GlobalVariable *GVar = dyn_cast<GlobalVariable>(GV);
  if (GVar == 0)
    return SectionKind::getText();

  
  // Handle thread-local data first.
  if (GVar->isThreadLocal()) {
    if (isSuitableForBSS(GVar))
      return SectionKind::getThreadBSS();
    return SectionKind::getThreadData();
  }

  // Variable can be easily put to BSS section.
  if (isSuitableForBSS(GVar))
    return SectionKind::getBSS();

  Constant *C = GVar->getInitializer();
  
  // If the global is marked constant, we can put it into a mergable section,
  // a mergable string section, or general .data if it contains relocations.
  if (GVar->isConstant()) {
    // If the initializer for the global contains something that requires a
    // relocation, then we may have to drop this into a wriable data section
    // even though it is marked const.
    switch (C->getRelocationInfo()) {
    default: llvm_unreachable("unknown relocation info kind");
    case Constant::NoRelocation:
      // If initializer is a null-terminated string, put it in a "cstring"
      // section of the right width.
      if (const ArrayType *ATy = dyn_cast<ArrayType>(C->getType())) {
        if (const IntegerType *ITy = 
              dyn_cast<IntegerType>(ATy->getElementType())) {
          if ((ITy->getBitWidth() == 8 || ITy->getBitWidth() == 16 ||
               ITy->getBitWidth() == 32) &&
              IsNullTerminatedString(C)) {
            if (ITy->getBitWidth() == 8)
              return SectionKind::getMergeable1ByteCString();
            if (ITy->getBitWidth() == 16)
              return SectionKind::getMergeable2ByteCString();
                                         
            assert(ITy->getBitWidth() == 32 && "Unknown width");
            return SectionKind::getMergeable4ByteCString();
          }
        }
      }
        
      // Otherwise, just drop it into a mergable constant section.  If we have
      // a section for this size, use it, otherwise use the arbitrary sized
      // mergable section.
      switch (TM.getTargetData()->getTypeAllocSize(C->getType())) {
      case 4:  return SectionKind::getMergeableConst4();
      case 8:  return SectionKind::getMergeableConst8();
      case 16: return SectionKind::getMergeableConst16();
      default: return SectionKind::getMergeableConst();
      }
      
    case Constant::LocalRelocation:
      // In static relocation model, the linker will resolve all addresses, so
      // the relocation entries will actually be constants by the time the app
      // starts up.  However, we can't put this into a mergable section, because
      // the linker doesn't take relocations into consideration when it tries to
      // merge entries in the section.
      if (ReloModel == Reloc::Static)
        return SectionKind::getReadOnly();
              
      // Otherwise, the dynamic linker needs to fix it up, put it in the
      // writable data.rel.local section.
      return SectionKind::getReadOnlyWithRelLocal();
              
    case Constant::GlobalRelocations:
      // In static relocation model, the linker will resolve all addresses, so
      // the relocation entries will actually be constants by the time the app
      // starts up.  However, we can't put this into a mergable section, because
      // the linker doesn't take relocations into consideration when it tries to
      // merge entries in the section.
      if (ReloModel == Reloc::Static)
        return SectionKind::getReadOnly();
      
      // Otherwise, the dynamic linker needs to fix it up, put it in the
      // writable data.rel section.
      return SectionKind::getReadOnlyWithRel();
    }
  }

  // Okay, this isn't a constant.  If the initializer for the global is going
  // to require a runtime relocation by the dynamic linker, put it into a more
  // specific section to improve startup time of the app.  This coalesces these
  // globals together onto fewer pages, improving the locality of the dynamic
  // linker.
  if (ReloModel == Reloc::Static)
    return SectionKind::getDataNoRel();

  switch (C->getRelocationInfo()) {
  default: llvm_unreachable("unknown relocation info kind");
  case Constant::NoRelocation:
    return SectionKind::getDataNoRel();
  case Constant::LocalRelocation:
    return SectionKind::getDataRelLocal();
  case Constant::GlobalRelocations:
    return SectionKind::getDataRel();
  }
}

/// SectionForGlobal - This method computes the appropriate section to emit
/// the specified global variable or function definition.  This should not
/// be passed external (or available externally) globals.
const MCSection *TargetLoweringObjectFile::
SectionForGlobal(const GlobalValue *GV, SectionKind Kind, Mangler *Mang,
                 const TargetMachine &TM) const {
  // Select section name.
  if (GV->hasSection())
    return getExplicitSectionGlobal(GV, Kind, Mang, TM);
  
  
  // Use default section depending on the 'type' of global
  return SelectSectionForGlobal(GV, Kind, Mang, TM);
}


// Lame default implementation. Calculate the section name for global.
const MCSection *
TargetLoweringObjectFile::SelectSectionForGlobal(const GlobalValue *GV,
                                                 SectionKind Kind,
                                                 Mangler *Mang,
                                                 const TargetMachine &TM) const{
  assert(!Kind.isThreadLocal() && "Doesn't support TLS");
  
  if (Kind.isText())
    return getTextSection();
  
  if (Kind.isBSS() && BSSSection != 0)
    return BSSSection;
  
  if (Kind.isReadOnly() && ReadOnlySection != 0)
    return ReadOnlySection;

  return getDataSection();
}

/// getSectionForConstant - Given a mergable constant with the
/// specified size and relocation information, return a section that it
/// should be placed in.
const MCSection *
TargetLoweringObjectFile::getSectionForConstant(SectionKind Kind) const {
  if (Kind.isReadOnly() && ReadOnlySection != 0)
    return ReadOnlySection;
  
  return DataSection;
}



//===----------------------------------------------------------------------===//
//                                  ELF
//===----------------------------------------------------------------------===//

const MCSection *TargetLoweringObjectFileELF::
getELFSection(const char *Name, bool isDirective, SectionKind Kind) const {
  if (MCSection *S = getContext().GetSection(Name))
    return S;
  return MCSection::Create(Name, isDirective, Kind, getContext());
}

void TargetLoweringObjectFileELF::Initialize(MCContext &Ctx,
                                             const TargetMachine &TM) {
  TargetLoweringObjectFile::Initialize(Ctx, TM);
  if (!HasCrazyBSS)
    BSSSection = getELFSection("\t.bss", true, SectionKind::getBSS());
  else
    // PPC/Linux doesn't support the .bss directive, it needs .section .bss.
    // FIXME: Does .section .bss work everywhere??
    // FIXME2: this should just be handle by the section printer.  We should get
    // away from syntactic view of the sections and MCSection should just be a
    // semantic view.
    BSSSection = getELFSection("\t.bss", false, SectionKind::getBSS());

    
  TextSection = getELFSection("\t.text", true, SectionKind::getText());
  DataSection = getELFSection("\t.data", true, SectionKind::getDataRel());
  ReadOnlySection =
    getELFSection("\t.rodata", false, SectionKind::getReadOnly());
  TLSDataSection =
    getELFSection("\t.tdata", false, SectionKind::getThreadData());
  
  TLSBSSSection = getELFSection("\t.tbss", false, 
                                     SectionKind::getThreadBSS());

  DataRelSection = getELFSection("\t.data.rel", false,
                                      SectionKind::getDataRel());
  DataRelLocalSection = getELFSection("\t.data.rel.local", false,
                                   SectionKind::getDataRelLocal());
  DataRelROSection = getELFSection("\t.data.rel.ro", false,
                                SectionKind::getReadOnlyWithRel());
  DataRelROLocalSection =
    getELFSection("\t.data.rel.ro.local", false,
                       SectionKind::getReadOnlyWithRelLocal());
    
  MergeableConst4Section = getELFSection(".rodata.cst4", false,
                                SectionKind::getMergeableConst4());
  MergeableConst8Section = getELFSection(".rodata.cst8", false,
                                SectionKind::getMergeableConst8());
  MergeableConst16Section = getELFSection(".rodata.cst16", false,
                               SectionKind::getMergeableConst16());

  StaticCtorSection =
    getELFSection(".ctors", false, SectionKind::getDataRel());
  StaticDtorSection =
    getELFSection(".dtors", false, SectionKind::getDataRel());
  
  // Exception Handling Sections.
  
  // FIXME: We're emitting LSDA info into a readonly section on ELF, even though
  // it contains relocatable pointers.  In PIC mode, this is probably a big
  // runtime hit for C++ apps.  Either the contents of the LSDA need to be
  // adjusted or this should be a data section.
  LSDASection =
    getELFSection(".gcc_except_table", false, SectionKind::getReadOnly());
  EHFrameSection =
    getELFSection(".eh_frame", false, SectionKind::getDataRel());
  
  // Debug Info Sections.
  DwarfAbbrevSection = 
    getELFSection(".debug_abbrev", false, SectionKind::getMetadata());
  DwarfInfoSection = 
    getELFSection(".debug_info", false, SectionKind::getMetadata());
  DwarfLineSection = 
    getELFSection(".debug_line", false, SectionKind::getMetadata());
  DwarfFrameSection = 
    getELFSection(".debug_frame", false, SectionKind::getMetadata());
  DwarfPubNamesSection = 
    getELFSection(".debug_pubnames", false, SectionKind::getMetadata());
  DwarfPubTypesSection = 
    getELFSection(".debug_pubtypes", false, SectionKind::getMetadata());
  DwarfStrSection = 
    getELFSection(".debug_str", false, SectionKind::getMetadata());
  DwarfLocSection = 
    getELFSection(".debug_loc", false, SectionKind::getMetadata());
  DwarfARangesSection = 
    getELFSection(".debug_aranges", false, SectionKind::getMetadata());
  DwarfRangesSection = 
    getELFSection(".debug_ranges", false, SectionKind::getMetadata());
  DwarfMacroInfoSection = 
    getELFSection(".debug_macinfo", false, SectionKind::getMetadata());
}


static SectionKind 
getELFKindForNamedSection(const char *Name, SectionKind K) {
  if (Name[0] != '.') return K;
  
  // Some lame default implementation based on some magic section names.
  if (strncmp(Name, ".gnu.linkonce.b.", 16) == 0 ||
      strncmp(Name, ".llvm.linkonce.b.", 17) == 0 ||
      strncmp(Name, ".gnu.linkonce.sb.", 17) == 0 ||
      strncmp(Name, ".llvm.linkonce.sb.", 18) == 0)
    return SectionKind::getBSS();
  
  if (strcmp(Name, ".tdata") == 0 ||
      strncmp(Name, ".tdata.", 7) == 0 ||
      strncmp(Name, ".gnu.linkonce.td.", 17) == 0 ||
      strncmp(Name, ".llvm.linkonce.td.", 18) == 0)
    return SectionKind::getThreadData();
  
  if (strcmp(Name, ".tbss") == 0 ||
      strncmp(Name, ".tbss.", 6) == 0 ||
      strncmp(Name, ".gnu.linkonce.tb.", 17) == 0 ||
      strncmp(Name, ".llvm.linkonce.tb.", 18) == 0)
    return SectionKind::getThreadBSS();
  
  return K;
}

const MCSection *TargetLoweringObjectFileELF::
getExplicitSectionGlobal(const GlobalValue *GV, SectionKind Kind, 
                         Mangler *Mang, const TargetMachine &TM) const {
  // Infer section flags from the section name if we can.
  Kind = getELFKindForNamedSection(GV->getSection().c_str(), Kind);
  
  return getELFSection(GV->getSection().c_str(), false, Kind);
}
      
      
      
void TargetLoweringObjectFileELF::
getSectionFlagsAsString(SectionKind Kind, SmallVectorImpl<char> &Str,
                        const TargetAsmInfo &TAI) const {
  // Handle the weird solaris syntax if desired.
  if (TAI.usesSunStyleELFSectionSwitchSyntax() &&
      !Kind.isMergeableConst() && !Kind.isMergeableCString()) {
    // FIXME: Inefficient.
    std::string Res;
    if (!Kind.isMetadata())
      Res += ",#alloc";
    if (Kind.isText())
      Res += ",#execinstr";
    if (Kind.isWriteable())
      Res += ",#write";
    if (Kind.isThreadLocal())
      Res += ",#tls";
    Str.append(Res.begin(), Res.end());
    return;    
  }
  
  Str.push_back(',');
  Str.push_back('"');
  
  if (!Kind.isMetadata())
    Str.push_back('a');
  if (Kind.isText())
    Str.push_back('x');
  if (Kind.isWriteable())
    Str.push_back('w');
  if (Kind.isMergeable1ByteCString() ||
      Kind.isMergeable2ByteCString() ||
      Kind.isMergeable4ByteCString() ||
      Kind.isMergeableConst4() ||
      Kind.isMergeableConst8() ||
      Kind.isMergeableConst16())
    Str.push_back('M');
  if (Kind.isMergeable1ByteCString() ||
      Kind.isMergeable2ByteCString() ||
      Kind.isMergeable4ByteCString())
    Str.push_back('S');
  if (Kind.isThreadLocal())
    Str.push_back('T');
  
  Str.push_back('"');
  Str.push_back(',');
  
  // If comment string is '@', e.g. as on ARM - use '%' instead
  if (AtIsCommentChar)
    Str.push_back('%');
  else
    Str.push_back('@');
  
  const char *KindStr;
  if (Kind.isBSS() || Kind.isThreadBSS())
    KindStr = "nobits";
  else
    KindStr = "progbits";
  
  Str.append(KindStr, KindStr+strlen(KindStr));
  
  if (Kind.isMergeable1ByteCString()) {
    Str.push_back(',');
    Str.push_back('1');
  } else if (Kind.isMergeable2ByteCString()) {
    Str.push_back(',');
    Str.push_back('2');
  } else if (Kind.isMergeable4ByteCString()) {
    Str.push_back(',');
    Str.push_back('4');
  } else if (Kind.isMergeableConst4()) {
    Str.push_back(',');
    Str.push_back('4');
  } else if (Kind.isMergeableConst8()) {
    Str.push_back(',');
    Str.push_back('8');
  } else if (Kind.isMergeableConst16()) {
    Str.push_back(',');
    Str.push_back('1');
    Str.push_back('6');
  }
}


static const char *getSectionPrefixForUniqueGlobal(SectionKind Kind) {
  if (Kind.isText())                 return ".gnu.linkonce.t.";
  if (Kind.isReadOnly())             return ".gnu.linkonce.r.";
  
  if (Kind.isThreadData())           return ".gnu.linkonce.td.";
  if (Kind.isThreadBSS())            return ".gnu.linkonce.tb.";
  
  if (Kind.isBSS())                  return ".gnu.linkonce.b.";
  if (Kind.isDataNoRel())            return ".gnu.linkonce.d.";
  if (Kind.isDataRelLocal())         return ".gnu.linkonce.d.rel.local.";
  if (Kind.isDataRel())              return ".gnu.linkonce.d.rel.";
  if (Kind.isReadOnlyWithRelLocal()) return ".gnu.linkonce.d.rel.ro.local.";
  
  assert(Kind.isReadOnlyWithRel() && "Unknown section kind");
  return ".gnu.linkonce.d.rel.ro.";
}

const MCSection *TargetLoweringObjectFileELF::
SelectSectionForGlobal(const GlobalValue *GV, SectionKind Kind,
                       Mangler *Mang, const TargetMachine &TM) const {
  
  // If this global is linkonce/weak and the target handles this by emitting it
  // into a 'uniqued' section name, create and return the section now.
  if (GV->isWeakForLinker()) {
    const char *Prefix = getSectionPrefixForUniqueGlobal(Kind);
    std::string Name = Mang->makeNameProper(GV->getNameStr());
    return getELFSection((Prefix+Name).c_str(), false, Kind);
  }
  
  if (Kind.isText()) return TextSection;
  
  if (Kind.isMergeable1ByteCString() ||
      Kind.isMergeable2ByteCString() ||
      Kind.isMergeable4ByteCString()) {
    
    // We also need alignment here.
    // FIXME: this is getting the alignment of the character, not the
    // alignment of the global!
    unsigned Align = 
      TM.getTargetData()->getPreferredAlignment(cast<GlobalVariable>(GV));
    
    const char *SizeSpec = ".rodata.str1.";
    if (Kind.isMergeable2ByteCString())
      SizeSpec = ".rodata.str2.";
    else if (Kind.isMergeable4ByteCString())
      SizeSpec = ".rodata.str4.";
    else
      assert(Kind.isMergeable1ByteCString() && "unknown string width");
    
    
    std::string Name = SizeSpec + utostr(Align);
    return getELFSection(Name.c_str(), false, Kind);
  }
  
  if (Kind.isMergeableConst()) {
    if (Kind.isMergeableConst4())
      return MergeableConst4Section;
    if (Kind.isMergeableConst8())
      return MergeableConst8Section;
    if (Kind.isMergeableConst16())
      return MergeableConst16Section;
    return ReadOnlySection;  // .const
  }
  
  if (Kind.isReadOnly())             return ReadOnlySection;
  
  if (Kind.isThreadData())           return TLSDataSection;
  if (Kind.isThreadBSS())            return TLSBSSSection;
  
  if (Kind.isBSS())                  return BSSSection;
  
  if (Kind.isDataNoRel())            return DataSection;
  if (Kind.isDataRelLocal())         return DataRelLocalSection;
  if (Kind.isDataRel())              return DataRelSection;
  if (Kind.isReadOnlyWithRelLocal()) return DataRelROLocalSection;
  
  assert(Kind.isReadOnlyWithRel() && "Unknown section kind");
  return DataRelROSection;
}

/// getSectionForConstant - Given a mergeable constant with the
/// specified size and relocation information, return a section that it
/// should be placed in.
const MCSection *TargetLoweringObjectFileELF::
getSectionForConstant(SectionKind Kind) const {
  if (Kind.isMergeableConst4())
    return MergeableConst4Section;
  if (Kind.isMergeableConst8())
    return MergeableConst8Section;
  if (Kind.isMergeableConst16())
    return MergeableConst16Section;
  if (Kind.isReadOnly())
    return ReadOnlySection;
  
  if (Kind.isReadOnlyWithRelLocal()) return DataRelROLocalSection;
  assert(Kind.isReadOnlyWithRel() && "Unknown section kind");
  return DataRelROSection;
}

//===----------------------------------------------------------------------===//
//                                 MachO
//===----------------------------------------------------------------------===//


const MCSection *TargetLoweringObjectFileMachO::
getMachOSection(const char *Name, bool isDirective, SectionKind Kind) const {
  if (MCSection *S = getContext().GetSection(Name))
    return S;
  return MCSection::Create(Name, isDirective, Kind, getContext());
}



void TargetLoweringObjectFileMachO::Initialize(MCContext &Ctx,
                                               const TargetMachine &TM) {
  TargetLoweringObjectFile::Initialize(Ctx, TM);
  TextSection = getMachOSection("\t.text", true, SectionKind::getText());
  DataSection = getMachOSection("\t.data", true, SectionKind::getDataRel());
  
  CStringSection = getMachOSection("\t.cstring", true,
                                   SectionKind::getMergeable1ByteCString());
  UStringSection = getMachOSection("__TEXT,__ustring", false,
                                   SectionKind::getMergeable2ByteCString());
  FourByteConstantSection = getMachOSection("\t.literal4\n", true,
                                            SectionKind::getMergeableConst4());
  EightByteConstantSection = getMachOSection("\t.literal8\n", true,
                                             SectionKind::getMergeableConst8());
  
  // ld_classic doesn't support .literal16 in 32-bit mode, and ld64 falls back
  // to using it in -static mode.
  if (TM.getRelocationModel() != Reloc::Static &&
      TM.getTargetData()->getPointerSize() == 32)
    SixteenByteConstantSection = 
      getMachOSection("\t.literal16\n", true, 
                      SectionKind::getMergeableConst16());
  else
    SixteenByteConstantSection = 0;
  
  ReadOnlySection = getMachOSection("\t.const", true,
                                    SectionKind::getReadOnly());
  
  TextCoalSection =
    getMachOSection("\t__TEXT,__textcoal_nt,coalesced,pure_instructions",
                    false, SectionKind::getText());
  ConstTextCoalSection = getMachOSection("\t__TEXT,__const_coal,coalesced",
                                         false, SectionKind::getText());
  ConstDataCoalSection = getMachOSection("\t__DATA,__const_coal,coalesced",
                                         false, SectionKind::getText());
  ConstDataSection = getMachOSection("\t.const_data", true,
                                     SectionKind::getReadOnlyWithRel());
  DataCoalSection = getMachOSection("\t__DATA,__datacoal_nt,coalesced",
                                    false, SectionKind::getDataRel());

  if (TM.getRelocationModel() == Reloc::Static) {
    StaticCtorSection =
      getMachOSection(".constructor", true, SectionKind::getDataRel());
    StaticDtorSection =
      getMachOSection(".destructor", true, SectionKind::getDataRel());
  } else {
    StaticCtorSection =
      getMachOSection(".mod_init_func", true, SectionKind::getDataRel());
    StaticDtorSection =
      getMachOSection(".mod_term_func", true, SectionKind::getDataRel());
  }
  
  // Exception Handling.
  LSDASection = getMachOSection("__DATA,__gcc_except_tab", false,
                                SectionKind::getDataRel());
  EHFrameSection =
    getMachOSection("__TEXT,__eh_frame,coalesced,no_toc+strip_static_syms"
                    "+live_support", false, SectionKind::getReadOnly());

  // Debug Information.
  // FIXME: Don't use 'directive' syntax: need flags for debug/regular??
  // FIXME: Need __DWARF segment.
  DwarfAbbrevSection = 
    getMachOSection(".section __DWARF,__debug_abbrev,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfInfoSection =  
    getMachOSection(".section __DWARF,__debug_info,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfLineSection =  
    getMachOSection(".section __DWARF,__debug_line,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfFrameSection =  
    getMachOSection(".section __DWARF,__debug_frame,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfPubNamesSection =  
    getMachOSection(".section __DWARF,__debug_pubnames,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfPubTypesSection =  
    getMachOSection(".section __DWARF,__debug_pubtypes,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfStrSection =  
    getMachOSection(".section __DWARF,__debug_str,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfLocSection =  
    getMachOSection(".section __DWARF,__debug_loc,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfARangesSection =  
    getMachOSection(".section __DWARF,__debug_aranges,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfRangesSection =  
    getMachOSection(".section __DWARF,__debug_ranges,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfMacroInfoSection =  
    getMachOSection(".section __DWARF,__debug_macinfo,regular,debug", true,
                    SectionKind::getMetadata());
  DwarfDebugInlineSection = 
    getMachOSection(".section __DWARF,__debug_inlined,regular,debug", true,
                    SectionKind::getMetadata());
}

const MCSection *TargetLoweringObjectFileMachO::
getExplicitSectionGlobal(const GlobalValue *GV, SectionKind Kind, 
                         Mangler *Mang, const TargetMachine &TM) const {
  return getMachOSection(GV->getSection().c_str(), false, Kind);
}

const MCSection *TargetLoweringObjectFileMachO::
SelectSectionForGlobal(const GlobalValue *GV, SectionKind Kind,
                       Mangler *Mang, const TargetMachine &TM) const {
  assert(!Kind.isThreadLocal() && "Darwin doesn't support TLS");
  
  if (Kind.isText())
    return GV->isWeakForLinker() ? TextCoalSection : TextSection;
  
  // If this is weak/linkonce, put this in a coalescable section, either in text
  // or data depending on if it is writable.
  if (GV->isWeakForLinker()) {
    if (Kind.isReadOnly())
      return ConstTextCoalSection;
    return DataCoalSection;
  }
  
  // FIXME: Alignment check should be handled by section classifier.
  if (Kind.isMergeable1ByteCString() ||
      Kind.isMergeable2ByteCString()) {
    if (TM.getTargetData()->getPreferredAlignment(
                                              cast<GlobalVariable>(GV)) < 32) {
      if (Kind.isMergeable1ByteCString())
        return CStringSection;
      assert(Kind.isMergeable2ByteCString());
      return UStringSection;
    }
  }
  
  if (Kind.isMergeableConst()) {
    if (Kind.isMergeableConst4())
      return FourByteConstantSection;
    if (Kind.isMergeableConst8())
      return EightByteConstantSection;
    if (Kind.isMergeableConst16() && SixteenByteConstantSection)
      return SixteenByteConstantSection;
  }

  // Otherwise, if it is readonly, but not something we can specially optimize,
  // just drop it in .const.
  if (Kind.isReadOnly())
    return ReadOnlySection;

  // If this is marked const, put it into a const section.  But if the dynamic
  // linker needs to write to it, put it in the data segment.
  if (Kind.isReadOnlyWithRel())
    return ConstDataSection;
  
  // Otherwise, just drop the variable in the normal data section.
  return DataSection;
}

const MCSection *
TargetLoweringObjectFileMachO::getSectionForConstant(SectionKind Kind) const {
  // If this constant requires a relocation, we have to put it in the data
  // segment, not in the text segment.
  if (Kind.isDataRel())
    return ConstDataSection;
  
  if (Kind.isMergeableConst4())
    return FourByteConstantSection;
  if (Kind.isMergeableConst8())
    return EightByteConstantSection;
  if (Kind.isMergeableConst16() && SixteenByteConstantSection)
    return SixteenByteConstantSection;
  return ReadOnlySection;  // .const
}

/// shouldEmitUsedDirectiveFor - This hook allows targets to selectively decide
/// not to emit the UsedDirective for some symbols in llvm.used.
// FIXME: REMOVE this (rdar://7071300)
bool TargetLoweringObjectFileMachO::
shouldEmitUsedDirectiveFor(const GlobalValue *GV, Mangler *Mang) const {
  /// On Darwin, internally linked data beginning with "L" or "l" does not have
  /// the directive emitted (this occurs in ObjC metadata).
  if (!GV) return false;
    
  // Check whether the mangled name has the "Private" or "LinkerPrivate" prefix.
  if (GV->hasLocalLinkage() && !isa<Function>(GV)) {
    // FIXME: ObjC metadata is currently emitted as internal symbols that have
    // \1L and \0l prefixes on them.  Fix them to be Private/LinkerPrivate and
    // this horrible hack can go away.
    const std::string &Name = Mang->getMangledName(GV);
    if (Name[0] == 'L' || Name[0] == 'l')
      return false;
  }
  
  return true;
}


//===----------------------------------------------------------------------===//
//                                  COFF
//===----------------------------------------------------------------------===//


const MCSection *TargetLoweringObjectFileCOFF::
getCOFFSection(const char *Name, bool isDirective, SectionKind Kind) const {
  if (MCSection *S = getContext().GetSection(Name))
    return S;
  return MCSection::Create(Name, isDirective, Kind, getContext());
}

void TargetLoweringObjectFileCOFF::Initialize(MCContext &Ctx,
                                              const TargetMachine &TM) {
  TargetLoweringObjectFile::Initialize(Ctx, TM);
  TextSection = getCOFFSection("\t.text", true, SectionKind::getText());
  DataSection = getCOFFSection("\t.data", true, SectionKind::getDataRel());
  StaticCtorSection =
    getCOFFSection(".ctors", false, SectionKind::getDataRel());
  StaticDtorSection =
    getCOFFSection(".dtors", false, SectionKind::getDataRel());
  
  
  // Debug info.
  // FIXME: Don't use 'directive' mode here.
  DwarfAbbrevSection =  
    getCOFFSection("\t.section\t.debug_abbrev,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfInfoSection =    
    getCOFFSection("\t.section\t.debug_info,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfLineSection =    
    getCOFFSection("\t.section\t.debug_line,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfFrameSection =   
    getCOFFSection("\t.section\t.debug_frame,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfPubNamesSection =
    getCOFFSection("\t.section\t.debug_pubnames,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfPubTypesSection =
    getCOFFSection("\t.section\t.debug_pubtypes,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfStrSection =     
    getCOFFSection("\t.section\t.debug_str,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfLocSection =     
    getCOFFSection("\t.section\t.debug_loc,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfARangesSection = 
    getCOFFSection("\t.section\t.debug_aranges,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfRangesSection =  
    getCOFFSection("\t.section\t.debug_ranges,\"dr\"",
                   true, SectionKind::getMetadata());
  DwarfMacroInfoSection = 
    getCOFFSection("\t.section\t.debug_macinfo,\"dr\"",
                   true, SectionKind::getMetadata());
}

const MCSection *TargetLoweringObjectFileCOFF::
getExplicitSectionGlobal(const GlobalValue *GV, SectionKind Kind, 
                         Mangler *Mang, const TargetMachine &TM) const {
  return getCOFFSection(GV->getSection().c_str(), false, Kind);
}


void TargetLoweringObjectFileCOFF::
getSectionFlagsAsString(SectionKind Kind, SmallVectorImpl<char> &Str,
                        const TargetAsmInfo &TAI) const {
  // FIXME: Inefficient.
  std::string Res = ",\"";
  if (Kind.isText())
    Res += 'x';
  if (Kind.isWriteable())
    Res += 'w';
  Res += "\"";
  
  Str.append(Res.begin(), Res.end());
}

static const char *getCOFFSectionPrefixForUniqueGlobal(SectionKind Kind) {
  if (Kind.isText())
    return ".text$linkonce";
  if (Kind.isWriteable())
    return ".data$linkonce";
  return ".rdata$linkonce";
}


const MCSection *TargetLoweringObjectFileCOFF::
SelectSectionForGlobal(const GlobalValue *GV, SectionKind Kind,
                       Mangler *Mang, const TargetMachine &TM) const {
  assert(!Kind.isThreadLocal() && "Doesn't support TLS");
  
  // If this global is linkonce/weak and the target handles this by emitting it
  // into a 'uniqued' section name, create and return the section now.
  if (GV->isWeakForLinker()) {
    const char *Prefix = getCOFFSectionPrefixForUniqueGlobal(Kind);
    std::string Name = Mang->makeNameProper(GV->getNameStr());
    return getCOFFSection((Prefix+Name).c_str(), false, Kind);
  }
  
  if (Kind.isText())
    return getTextSection();
  
  return getDataSection();
}

