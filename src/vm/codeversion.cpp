// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
// ===========================================================================
// File: CodeVersion.cpp
//
// ===========================================================================

#include "common.h"
#include "codeversion.h"

#ifdef FEATURE_CODE_VERSIONING
#include "threadsuspend.h"
#include "methoditer.h"
#include "../debug/ee/debugger.h"
#include "../debug/ee/walker.h"
#include "../debug/ee/controller.h"
#endif // FEATURE_CODE_VERSIONING

#ifndef FEATURE_CODE_VERSIONING

//
// When not using code versioning we've got a minimal implementation of 
// NativeCodeVersion that simply wraps a MethodDesc* with no additional
// versioning information
//

NativeCodeVersion::NativeCodeVersion() : m_pMethodDesc(PTR_NULL) {};
NativeCodeVersion::NativeCodeVersion(const NativeCodeVersion & rhs) : m_pMethodDesc(rhs.m_pMethodDesc) {}
NativeCodeVersion::NativeCodeVersion(PTR_MethodDesc pMethod) : m_pMethodDesc(pMethod) {}
BOOL NativeCodeVersion::IsNull() const { return m_pMethodDesc == NULL; }
PTR_MethodDesc NativeCodeVersion::GetMethodDesc() const { return m_pMethodDesc; }
NativeCodeVersionId NativeCodeVersion::GetVersionId() const { return 0; }
BOOL NativeCodeVersion::IsDefaultVersion() const { return TRUE; }
PCODE NativeCodeVersion::GetNativeCode() const { return m_pMethodDesc->GetNativeCode(); }

#ifndef DACCESS_COMPILE
BOOL NativeCodeVersion::SetNativeCodeInterlocked(PCODE pCode, PCODE pExpected) { return m_pMethodDesc->SetNativeCodeInterlocked(pCode, pExpected); }
#endif

#ifdef HAVE_GCCOVER
PTR_GCCoverageInfo NativeCodeVersion::GetGCCoverageInfo() const { return GetMethodDesc()->m_GcCover; }
void NativeCodeVersion::SetGCCoverageInfo(PTR_GCCoverageInfo gcCover)
{
    MethodDesc *pMD = GetMethodDesc();
    _ASSERTE(gcCover == NULL || pMD->m_GcCover == NULL);
    *EnsureWritablePages(&pMD->m_GcCover) = gcCover;
}
#endif

bool NativeCodeVersion::operator==(const NativeCodeVersion & rhs) const { return m_pMethodDesc == rhs.m_pMethodDesc; }
bool NativeCodeVersion::operator!=(const NativeCodeVersion & rhs) const { return !operator==(rhs); }


#else // FEATURE_CODE_VERSIONING


// This HRESULT is only used as a private implementation detail. If it escapes through public APIS
// it is a bug. Corerror.xml has a comment in it reserving this value for our use but it doesn't
// appear in the public headers.

#define CORPROF_E_RUNTIME_SUSPEND_REQUIRED _HRESULT_TYPEDEF_(0x80131381L)

#ifndef DACCESS_COMPILE
NativeCodeVersionNode::NativeCodeVersionNode(
    NativeCodeVersionId id,
    MethodDesc* pMethodDesc,
    ReJITID parentId,
    NativeCodeVersion::OptimizationTier optimizationTier)
    :
    m_pNativeCode(NULL),
    m_pMethodDesc(pMethodDesc),
    m_parentId(parentId),
    m_pNextMethodDescSibling(NULL),
    m_id(id),
#ifdef FEATURE_TIERED_COMPILATION
    m_optTier(optimizationTier),
#endif
#ifdef HAVE_GCCOVER
    m_gcCover(PTR_NULL),
#endif
    m_flags(0)
{}
#endif

#ifdef DEBUG
BOOL NativeCodeVersionNode::LockOwnedByCurrentThread() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return GetMethodDesc()->GetCodeVersionManager()->LockOwnedByCurrentThread();
}
#endif //DEBUG

PTR_MethodDesc NativeCodeVersionNode::GetMethodDesc() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pMethodDesc;
}

PCODE NativeCodeVersionNode::GetNativeCode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pNativeCode;
}

ReJITID NativeCodeVersionNode::GetILVersionId() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_parentId;
}

ILCodeVersion NativeCodeVersionNode::GetILCodeVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef DEBUG
    if (GetILVersionId() != 0)
    {
        _ASSERTE(LockOwnedByCurrentThread());
    }
#endif
    PTR_MethodDesc pMD = GetMethodDesc();
    return pMD->GetCodeVersionManager()->GetILCodeVersion(pMD, GetILVersionId());
}

NativeCodeVersionId NativeCodeVersionNode::GetVersionId() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_id;
}

#ifndef DACCESS_COMPILE
BOOL NativeCodeVersionNode::SetNativeCodeInterlocked(PCODE pCode, PCODE pExpected)
{
    LIMITED_METHOD_CONTRACT;
    return FastInterlockCompareExchangePointer(&m_pNativeCode,
        (TADDR&)pCode, (TADDR&)pExpected) == (TADDR&)pExpected;
}
#endif

BOOL NativeCodeVersionNode::IsActiveChildVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return (m_flags & IsActiveChildFlag) != 0;
}

#ifndef DACCESS_COMPILE
void NativeCodeVersionNode::SetActiveChildFlag(BOOL isActive)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    if (isActive)
    {
        m_flags |= IsActiveChildFlag;
    }
    else
    {
        m_flags &= ~IsActiveChildFlag;
    }
}
#endif


#ifdef FEATURE_TIERED_COMPILATION

NativeCodeVersion::OptimizationTier NativeCodeVersionNode::GetOptimizationTier() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_optTier;
}

#ifndef DACCESS_COMPILE
void NativeCodeVersionNode::SetOptimizationTier(NativeCodeVersion::OptimizationTier tier)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(tier >= m_optTier);

    m_optTier = tier;
}
#endif

#endif // FEATURE_TIERED_COMPILATION

#ifdef HAVE_GCCOVER

PTR_GCCoverageInfo NativeCodeVersionNode::GetGCCoverageInfo() const
{
    LIMITED_METHOD_CONTRACT;
    return m_gcCover;
}

void NativeCodeVersionNode::SetGCCoverageInfo(PTR_GCCoverageInfo gcCover)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(gcCover == NULL || m_gcCover == NULL);

    m_gcCover = gcCover;
}

#endif // HAVE_GCCOVER

NativeCodeVersion::NativeCodeVersion() :
    m_storageKind(StorageKind::Unknown), m_pVersionNode(PTR_NULL)
{}

NativeCodeVersion::NativeCodeVersion(const NativeCodeVersion & rhs) :
    m_storageKind(rhs.m_storageKind)
{
    if(m_storageKind == StorageKind::Explicit)
    {
        m_pVersionNode = rhs.m_pVersionNode; 
    }
    else if(m_storageKind == StorageKind::Synthetic)
    {
        m_synthetic = rhs.m_synthetic;
    }
}

NativeCodeVersion::NativeCodeVersion(PTR_NativeCodeVersionNode pVersionNode) :
    m_storageKind(pVersionNode != NULL ? StorageKind::Explicit : StorageKind::Unknown),
    m_pVersionNode(pVersionNode)
{}

NativeCodeVersion::NativeCodeVersion(PTR_MethodDesc pMethod) :
    m_storageKind(pMethod != NULL ? StorageKind::Synthetic : StorageKind::Unknown)
{
    LIMITED_METHOD_DAC_CONTRACT;
    m_synthetic.m_pMethodDesc = pMethod;
}

BOOL NativeCodeVersion::IsNull() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_storageKind == StorageKind::Unknown;
}

BOOL NativeCodeVersion::IsDefaultVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_storageKind == StorageKind::Synthetic;
}

PTR_MethodDesc NativeCodeVersion::GetMethodDesc() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetMethodDesc();
    }
    else
    {
        return m_synthetic.m_pMethodDesc;
    }
}

PCODE NativeCodeVersion::GetNativeCode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetNativeCode();
    }
    else
    {
        return GetMethodDesc()->GetNativeCode();
    }
}

ReJITID NativeCodeVersion::GetILCodeVersionId() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetILVersionId();
    }
    else
    {
        return 0;
    }
}

ILCodeVersion NativeCodeVersion::GetILCodeVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetILCodeVersion();
    }
    else
    {
        PTR_MethodDesc pMethod = GetMethodDesc();
        return ILCodeVersion(dac_cast<PTR_Module>(pMethod->GetModule()), pMethod->GetMemberDef());
    }
}

NativeCodeVersionId NativeCodeVersion::GetVersionId() const 
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetVersionId();
    }
    else
    {
        return 0;
    }
}

#ifndef DACCESS_COMPILE
BOOL NativeCodeVersion::SetNativeCodeInterlocked(PCODE pCode, PCODE pExpected)
{
    LIMITED_METHOD_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->SetNativeCodeInterlocked(pCode, pExpected);
    }
    else
    {
        return GetMethodDesc()->SetNativeCodeInterlocked(pCode, pExpected);
    }
}
#endif

BOOL NativeCodeVersion::IsActiveChildVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->IsActiveChildVersion();
    }
    else
    {
        MethodDescVersioningState* pMethodVersioningState = GetMethodDescVersioningState();
        if (pMethodVersioningState == NULL)
        {
            return TRUE;
        }
        return pMethodVersioningState->IsDefaultVersionActiveChild();
    }
}

PTR_MethodDescVersioningState NativeCodeVersion::GetMethodDescVersioningState() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    PTR_MethodDesc pMethodDesc = GetMethodDesc();
    CodeVersionManager* pCodeVersionManager = pMethodDesc->GetCodeVersionManager();
    return pCodeVersionManager->GetMethodDescVersioningState(pMethodDesc);
}

#ifndef DACCESS_COMPILE
void NativeCodeVersion::SetActiveChildFlag(BOOL isActive)
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        AsNode()->SetActiveChildFlag(isActive);
    }
    else
    {
        MethodDescVersioningState* pMethodVersioningState = GetMethodDescVersioningState();
        pMethodVersioningState->SetDefaultVersionActiveChildFlag(isActive);
    }
}

MethodDescVersioningState* NativeCodeVersion::GetMethodDescVersioningState()
{
    LIMITED_METHOD_DAC_CONTRACT;
    MethodDesc* pMethodDesc = GetMethodDesc();
    CodeVersionManager* pCodeVersionManager = pMethodDesc->GetCodeVersionManager();
    return pCodeVersionManager->GetMethodDescVersioningState(pMethodDesc);
}
#endif

#ifdef FEATURE_TIERED_COMPILATION

NativeCodeVersion::OptimizationTier NativeCodeVersion::GetOptimizationTier() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetOptimizationTier();
    }
    else
    {
        return TieredCompilationManager::GetInitialOptimizationTier(GetMethodDesc());
    }
}

#ifndef DACCESS_COMPILE
void NativeCodeVersion::SetOptimizationTier(OptimizationTier tier)
{
    WRAPPER_NO_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        AsNode()->SetOptimizationTier(tier);
    }
    else
    {
        // State changes should have been made previously such that the initial tier is the new tier
        _ASSERTE(TieredCompilationManager::GetInitialOptimizationTier(GetMethodDesc()) == tier);
    }
}
#endif

#endif

#ifdef HAVE_GCCOVER

PTR_GCCoverageInfo NativeCodeVersion::GetGCCoverageInfo() const
{
    WRAPPER_NO_CONTRACT;

    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetGCCoverageInfo();
    }
    else
    {
        return GetMethodDesc()->m_GcCover;
    }
}

void NativeCodeVersion::SetGCCoverageInfo(PTR_GCCoverageInfo gcCover)
{
    WRAPPER_NO_CONTRACT;

    if (m_storageKind == StorageKind::Explicit)
    {
        AsNode()->SetGCCoverageInfo(gcCover);
    }
    else
    {
        MethodDesc *pMD = GetMethodDesc();
        _ASSERTE(gcCover == NULL || pMD->m_GcCover == NULL);
        *EnsureWritablePages(&pMD->m_GcCover) = gcCover;
    }
}

#endif // HAVE_GCCOVER

PTR_NativeCodeVersionNode NativeCodeVersion::AsNode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return m_pVersionNode;
    }
    else
    {
        return NULL;
    }
}

#ifndef DACCESS_COMPILE
PTR_NativeCodeVersionNode NativeCodeVersion::AsNode()
{
    LIMITED_METHOD_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return m_pVersionNode;
    }
    else
    {
        return NULL;
    }
}
#endif

bool NativeCodeVersion::operator==(const NativeCodeVersion & rhs) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return (rhs.m_storageKind == StorageKind::Explicit) &&
            (rhs.AsNode() == AsNode());
    }
    else if (m_storageKind == StorageKind::Synthetic)
    {
        return (rhs.m_storageKind == StorageKind::Synthetic) &&
            (m_synthetic.m_pMethodDesc == rhs.m_synthetic.m_pMethodDesc);
    }
    else
    {
        return rhs.m_storageKind == StorageKind::Unknown;
    }
}
bool NativeCodeVersion::operator!=(const NativeCodeVersion & rhs) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return !operator==(rhs);
}

NativeCodeVersionCollection::NativeCodeVersionCollection(PTR_MethodDesc pMethodDescFilter, ILCodeVersion ilCodeFilter) :
    m_pMethodDescFilter(pMethodDescFilter),
    m_ilCodeFilter(ilCodeFilter)
{
}

NativeCodeVersionIterator NativeCodeVersionCollection::Begin()
{
    LIMITED_METHOD_DAC_CONTRACT;
    return NativeCodeVersionIterator(this);
}
NativeCodeVersionIterator NativeCodeVersionCollection::End()
{
    LIMITED_METHOD_DAC_CONTRACT;
    return NativeCodeVersionIterator(NULL);
}

NativeCodeVersionIterator::NativeCodeVersionIterator(NativeCodeVersionCollection* pNativeCodeVersionCollection) :
    m_stage(IterationStage::Initial),
    m_pCollection(pNativeCodeVersionCollection),
    m_pLinkedListCur(dac_cast<PTR_NativeCodeVersionNode>(nullptr))
{
    LIMITED_METHOD_DAC_CONTRACT;
    First();
}
void NativeCodeVersionIterator::First()
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_pCollection == NULL)
    {
        m_stage = IterationStage::End;
    }
    Next();
}
void NativeCodeVersionIterator::Next()
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_stage == IterationStage::Initial)
    {
        ILCodeVersion ilCodeFilter = m_pCollection->m_ilCodeFilter;
        m_stage = IterationStage::ImplicitCodeVersion;
        if (ilCodeFilter.IsNull() || ilCodeFilter.IsDefaultVersion())
        {
            m_cur = NativeCodeVersion(m_pCollection->m_pMethodDescFilter);
            return;
        }
    }
    if (m_stage == IterationStage::ImplicitCodeVersion)
    {
        m_stage = IterationStage::LinkedList;
        CodeVersionManager* pCodeVersionManager = m_pCollection->m_pMethodDescFilter->GetCodeVersionManager();
        MethodDescVersioningState* pMethodDescVersioningState = pCodeVersionManager->GetMethodDescVersioningState(m_pCollection->m_pMethodDescFilter);
        if (pMethodDescVersioningState == NULL)
        {
            m_pLinkedListCur = NULL;
        }
        else
        {
            ILCodeVersion ilCodeFilter = m_pCollection->m_ilCodeFilter;
            m_pLinkedListCur = pMethodDescVersioningState->GetFirstVersionNode();
            while (m_pLinkedListCur != NULL && !ilCodeFilter.IsNull() && ilCodeFilter.GetVersionId() != m_pLinkedListCur->GetILVersionId())
            {
                m_pLinkedListCur = m_pLinkedListCur->m_pNextMethodDescSibling;
            }
        }
        if (m_pLinkedListCur != NULL)
        {
            m_cur = NativeCodeVersion(m_pLinkedListCur);
            return;
        }
    }
    if (m_stage == IterationStage::LinkedList)
    {
        if (m_pLinkedListCur != NULL)
        {
            ILCodeVersion ilCodeFilter = m_pCollection->m_ilCodeFilter;
            do
            {
                m_pLinkedListCur = m_pLinkedListCur->m_pNextMethodDescSibling;
            } while (m_pLinkedListCur != NULL && !ilCodeFilter.IsNull() && ilCodeFilter.GetVersionId() != m_pLinkedListCur->GetILVersionId());
        }
        if (m_pLinkedListCur != NULL)
        {
            m_cur = NativeCodeVersion(m_pLinkedListCur);
            return;
        }
        else
        {
            m_stage = IterationStage::End;
            m_cur = NativeCodeVersion();
        }
    }
}
const NativeCodeVersion & NativeCodeVersionIterator::Get() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_cur;
}
bool NativeCodeVersionIterator::Equal(const NativeCodeVersionIterator &i) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_cur == i.m_cur;
}

ILCodeVersionNode::ILCodeVersionNode() :
    m_pModule(dac_cast<PTR_Module>(nullptr)),
    m_methodDef(0),
    m_rejitId(0),
    m_pNextILVersionNode(dac_cast<PTR_ILCodeVersionNode>(nullptr)),
    m_rejitState(ILCodeVersion::kStateRequested),
    m_pIL(),
    m_jitFlags(0)
{
    m_pIL.Store(dac_cast<PTR_COR_ILMETHOD>(nullptr));
}

#ifndef DACCESS_COMPILE
ILCodeVersionNode::ILCodeVersionNode(Module* pModule, mdMethodDef methodDef, ReJITID id) :
    m_pModule(pModule),
    m_methodDef(methodDef),
    m_rejitId(id),
    m_pNextILVersionNode(dac_cast<PTR_ILCodeVersionNode>(nullptr)),
    m_rejitState(ILCodeVersion::kStateRequested),
    m_pIL(nullptr),
    m_jitFlags(0)
{}
#endif

#ifdef DEBUG
BOOL ILCodeVersionNode::LockOwnedByCurrentThread() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return GetModule()->GetCodeVersionManager()->LockOwnedByCurrentThread();
}
#endif //DEBUG

PTR_Module ILCodeVersionNode::GetModule() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pModule;
}

mdMethodDef ILCodeVersionNode::GetMethodDef() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_methodDef;
}

ReJITID ILCodeVersionNode::GetVersionId() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_rejitId;
}

ILCodeVersion::RejitFlags ILCodeVersionNode::GetRejitState() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return static_cast<ILCodeVersion::RejitFlags>(m_rejitState.Load() & ILCodeVersion::kStateMask);
}

BOOL ILCodeVersionNode::GetEnableReJITCallback() const
{
    LIMITED_METHOD_DAC_CONTRACT;

    return (m_rejitState.Load() & ILCodeVersion::kSuppressParams) == ILCodeVersion::kSuppressParams;
}

PTR_COR_ILMETHOD ILCodeVersionNode::GetIL() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return dac_cast<PTR_COR_ILMETHOD>(m_pIL.Load());
}

DWORD ILCodeVersionNode::GetJitFlags() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_jitFlags.Load();
}

const InstrumentedILOffsetMapping* ILCodeVersionNode::GetInstrumentedILMap() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return &m_instrumentedILMap;
}

PTR_ILCodeVersionNode ILCodeVersionNode::GetNextILVersionNode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return m_pNextILVersionNode;
}

#ifndef DACCESS_COMPILE
void ILCodeVersionNode::SetRejitState(ILCodeVersion::RejitFlags newState)
{
    LIMITED_METHOD_CONTRACT;
    // We're doing a non thread safe modification to m_rejitState
    _ASSERTE(LockOwnedByCurrentThread());

    ILCodeVersion::RejitFlags oldNonMaskFlags = 
        static_cast<ILCodeVersion::RejitFlags>(m_rejitState.Load() & ~ILCodeVersion::kStateMask);
    m_rejitState.Store(static_cast<ILCodeVersion::RejitFlags>(newState | oldNonMaskFlags));
}

void ILCodeVersionNode::SetEnableReJITCallback(BOOL state)
{
    LIMITED_METHOD_CONTRACT;
    // We're doing a non thread safe modification to m_rejitState
    _ASSERTE(LockOwnedByCurrentThread());

    ILCodeVersion::RejitFlags oldFlags = m_rejitState.Load();
    if (state)
    {
        m_rejitState.Store(static_cast<ILCodeVersion::RejitFlags>(oldFlags | ILCodeVersion::kSuppressParams));
    }
    else
    {
        m_rejitState.Store(static_cast<ILCodeVersion::RejitFlags>(oldFlags & ~ILCodeVersion::kSuppressParams));
    }
}

void ILCodeVersionNode::SetIL(COR_ILMETHOD* pIL)
{
    LIMITED_METHOD_CONTRACT;
    m_pIL.Store(pIL);
}

void ILCodeVersionNode::SetJitFlags(DWORD flags)
{
    LIMITED_METHOD_CONTRACT;
    m_jitFlags.Store(flags);
}

void ILCodeVersionNode::SetInstrumentedILMap(SIZE_T cMap, COR_IL_MAP * rgMap)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    m_instrumentedILMap.SetMappingInfo(cMap, rgMap);
}

void ILCodeVersionNode::SetNextILVersionNode(ILCodeVersionNode* pNextILVersionNode)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    m_pNextILVersionNode = pNextILVersionNode;
}
#endif

ILCodeVersion::ILCodeVersion() :
    m_storageKind(StorageKind::Unknown)
{}

ILCodeVersion::ILCodeVersion(const ILCodeVersion & ilCodeVersion) :
    m_storageKind(ilCodeVersion.m_storageKind)
{
    if(m_storageKind == StorageKind::Explicit)
    {
        m_pVersionNode = ilCodeVersion.m_pVersionNode;
    }
    else if(m_storageKind == StorageKind::Synthetic)
    {
        m_synthetic = ilCodeVersion.m_synthetic;
    }
}

ILCodeVersion::ILCodeVersion(PTR_ILCodeVersionNode pILCodeVersionNode) :
    m_storageKind(pILCodeVersionNode != NULL ? StorageKind::Explicit : StorageKind::Unknown),
    m_pVersionNode(pILCodeVersionNode)
{}

ILCodeVersion::ILCodeVersion(PTR_Module pModule, mdMethodDef methodDef) :
    m_storageKind(pModule != NULL ? StorageKind::Synthetic : StorageKind::Unknown)
{
    LIMITED_METHOD_DAC_CONTRACT;
    m_synthetic.m_pModule = pModule;
    m_synthetic.m_methodDef = methodDef;
}

bool ILCodeVersion::operator==(const ILCodeVersion & rhs) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return (rhs.m_storageKind == StorageKind::Explicit) &&
            (AsNode() == rhs.AsNode());
    }
    else if (m_storageKind == StorageKind::Synthetic)
    {
        return (rhs.m_storageKind == StorageKind::Synthetic) &&
            (m_synthetic.m_pModule == rhs.m_synthetic.m_pModule) &&
            (m_synthetic.m_methodDef == rhs.m_synthetic.m_methodDef);
    }
    else
    {
        return rhs.m_storageKind == StorageKind::Unknown;
    }
}

BOOL ILCodeVersion::HasDefaultIL() const
{
    LIMITED_METHOD_CONTRACT;

    return (m_storageKind == StorageKind::Synthetic) || (AsNode()->GetIL() == NULL);
}

BOOL ILCodeVersion::IsNull() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_storageKind == StorageKind::Unknown;
}

BOOL ILCodeVersion::IsDefaultVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_storageKind == StorageKind::Synthetic;
}

PTR_Module ILCodeVersion::GetModule() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetModule();
    }
    else
    {
        return m_synthetic.m_pModule;
    }
}

mdMethodDef ILCodeVersion::GetMethodDef() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetMethodDef();
    }
    else
    {
        return m_synthetic.m_methodDef;
    }
}

ReJITID ILCodeVersion::GetVersionId() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetVersionId();
    }
    else
    {
        return 0;
    }
}

NativeCodeVersionCollection ILCodeVersion::GetNativeCodeVersions(PTR_MethodDesc pClosedMethodDesc) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return NativeCodeVersionCollection(pClosedMethodDesc, *this);
}

NativeCodeVersion ILCodeVersion::GetActiveNativeCodeVersion(PTR_MethodDesc pClosedMethodDesc) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    NativeCodeVersionCollection versions = GetNativeCodeVersions(pClosedMethodDesc);
    for (NativeCodeVersionIterator cur = versions.Begin(), end = versions.End(); cur != end; cur++)
    {
        if (cur->IsActiveChildVersion())
        {
            return *cur;
        }
    }
    return NativeCodeVersion();
}

ILCodeVersion::RejitFlags ILCodeVersion::GetRejitState() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetRejitState();
    }
    else
    {
        return ILCodeVersion::kStateActive;
    }
}

BOOL ILCodeVersion::GetEnableReJITCallback() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetEnableReJITCallback();
    }
    else
    {
        return FALSE;
    }
}

PTR_COR_ILMETHOD ILCodeVersion::GetIL() const
{
    CONTRACTL
    {
        THROWS; //GetILHeader throws
        GC_NOTRIGGER;
        FORBID_FAULT;
        MODE_ANY;
    }
    CONTRACTL_END

    PTR_COR_ILMETHOD pIL = NULL;
    if (m_storageKind == StorageKind::Explicit)
    {
        pIL = AsNode()->GetIL();
    }
    
    // For the default code version we always fetch the globally stored default IL for a method
    //
    // In the non-default code version we assume NULL is the equivalent of explicitly requesting to
    // re-use the default IL. Ideally there would be no reason to create a new version that re-uses
    // the default IL (just use the default code version for that) but we do it here for compat. We've 
    // got some profilers that use ReJIT to create a new code version and then instead of calling
    // ICorProfilerFunctionControl::SetILFunctionBody they call ICorProfilerInfo::SetILFunctionBody. 
    // This mutates the default IL so that it is now correct for their new code version. Of course this
    // also overwrote the previous default IL so now the default code version GetIL() is out of sync
    // with the jitted code. In the majority of cases we never re-read the IL after the initial
    // jitting so this issue goes unnoticed.
    //
    // If changing the default IL after it is in use becomes more problematic in the future we would
    // need to add enforcement that prevents profilers from using ICorProfilerInfo::SetILFunctionBody
    // that way + coordinate with them because it is a breaking change for any profiler currently doing it.
    if(pIL == NULL)
    {
        PTR_Module pModule = GetModule();
        PTR_MethodDesc pMethodDesc = dac_cast<PTR_MethodDesc>(pModule->LookupMethodDef(GetMethodDef()));
        if (pMethodDesc != NULL)
        {
            pIL = dac_cast<PTR_COR_ILMETHOD>(pMethodDesc->GetILHeader(TRUE));
        }
    }

    return pIL;
}

PTR_COR_ILMETHOD ILCodeVersion::GetILNoThrow() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    PTR_COR_ILMETHOD ret;
    EX_TRY
    {
        ret = GetIL();
    }
    EX_CATCH
    {
        ret = NULL;
    }
    EX_END_CATCH(RethrowTerminalExceptions);
    return ret;
}

DWORD ILCodeVersion::GetJitFlags() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetJitFlags();
    }
    else
    {
        return 0;
    }
}

const InstrumentedILOffsetMapping* ILCodeVersion::GetInstrumentedILMap() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_storageKind == StorageKind::Explicit)
    {
        return AsNode()->GetInstrumentedILMap();
    }
    else
    {
        return NULL;
    }
}

#ifndef DACCESS_COMPILE
void ILCodeVersion::SetRejitState(RejitFlags newState)
{
    LIMITED_METHOD_CONTRACT;
    AsNode()->SetRejitState(newState);
}

void ILCodeVersion::SetEnableReJITCallback(BOOL state)
{
    LIMITED_METHOD_CONTRACT;
    return AsNode()->SetEnableReJITCallback(state);
}

void ILCodeVersion::SetIL(COR_ILMETHOD* pIL)
{
    LIMITED_METHOD_CONTRACT;
    AsNode()->SetIL(pIL);
}

void ILCodeVersion::SetJitFlags(DWORD flags)
{
    LIMITED_METHOD_CONTRACT;
    AsNode()->SetJitFlags(flags);
}

void ILCodeVersion::SetInstrumentedILMap(SIZE_T cMap, COR_IL_MAP * rgMap)
{
    LIMITED_METHOD_CONTRACT;
    AsNode()->SetInstrumentedILMap(cMap, rgMap);
}

HRESULT ILCodeVersion::AddNativeCodeVersion(
    MethodDesc* pClosedMethodDesc,
    NativeCodeVersion::OptimizationTier optimizationTier,
    NativeCodeVersion* pNativeCodeVersion)
{
    LIMITED_METHOD_CONTRACT;
    CodeVersionManager* pManager = GetModule()->GetCodeVersionManager();
    HRESULT hr = pManager->AddNativeCodeVersion(*this, pClosedMethodDesc, optimizationTier, pNativeCodeVersion);
    if (FAILED(hr))
    {
        _ASSERTE(hr == E_OUTOFMEMORY);
        return hr;
    }
    return S_OK;
}

HRESULT ILCodeVersion::GetOrCreateActiveNativeCodeVersion(MethodDesc* pClosedMethodDesc, NativeCodeVersion* pActiveNativeCodeVersion)
{
    LIMITED_METHOD_CONTRACT;
    HRESULT hr = S_OK;
    NativeCodeVersion activeNativeChild = GetActiveNativeCodeVersion(pClosedMethodDesc);
    if (activeNativeChild.IsNull())
    {
        NativeCodeVersion::OptimizationTier optimizationTier =
            TieredCompilationManager::GetInitialOptimizationTier(pClosedMethodDesc);
        if (FAILED(hr = AddNativeCodeVersion(pClosedMethodDesc, optimizationTier, &activeNativeChild)))
        {
            _ASSERTE(hr == E_OUTOFMEMORY);
            return hr;
        }
    }
    // The first added child should automatically become active
    _ASSERTE(GetActiveNativeCodeVersion(pClosedMethodDesc) == activeNativeChild);
    *pActiveNativeCodeVersion = activeNativeChild;
    return S_OK;
}

HRESULT ILCodeVersion::SetActiveNativeCodeVersion(NativeCodeVersion activeNativeCodeVersion, BOOL fEESuspended)
{
    LIMITED_METHOD_CONTRACT;
    HRESULT hr = S_OK;
    MethodDesc* pMethodDesc = activeNativeCodeVersion.GetMethodDesc();
    NativeCodeVersion prevActiveVersion = GetActiveNativeCodeVersion(pMethodDesc);
    if (prevActiveVersion == activeNativeCodeVersion)
    {
        //nothing to do, this version is already active
        return S_OK;
    }

    if (!prevActiveVersion.IsNull())
    {
        prevActiveVersion.SetActiveChildFlag(FALSE);
    }
    activeNativeCodeVersion.SetActiveChildFlag(TRUE);

    // If needed update the published code body for this method
    CodeVersionManager* pCodeVersionManager = GetModule()->GetCodeVersionManager();
    if (pCodeVersionManager->GetActiveILCodeVersion(GetModule(), GetMethodDef()) == *this)
    {
        if (FAILED(hr = pCodeVersionManager->PublishNativeCodeVersion(pMethodDesc, activeNativeCodeVersion, fEESuspended)))
        {
            return hr;
        }
    }

    return S_OK;
}

ILCodeVersionNode* ILCodeVersion::AsNode()
{
    LIMITED_METHOD_CONTRACT;
    //This is dangerous - NativeCodeVersion coerces non-explicit versions to NULL but ILCodeVersion assumes the caller
    //will never invoke AsNode() on a non-explicit node. Asserting for now as a minimal fix, but we should revisit this.
    _ASSERTE(m_storageKind == StorageKind::Explicit);
    return m_pVersionNode;
}
#endif //DACCESS_COMPILE

PTR_ILCodeVersionNode ILCodeVersion::AsNode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    //This is dangerous - NativeCodeVersion coerces non-explicit versions to NULL but ILCodeVersion assumes the caller
    //will never invoke AsNode() on a non-explicit node. Asserting for now as a minimal fix, but we should revisit this.
    _ASSERTE(m_storageKind == StorageKind::Explicit);
    return m_pVersionNode;
}

ILCodeVersionCollection::ILCodeVersionCollection(PTR_Module pModule, mdMethodDef methodDef) :
    m_pModule(pModule),
    m_methodDef(methodDef)
{}

ILCodeVersionIterator ILCodeVersionCollection::Begin()
{
    LIMITED_METHOD_DAC_CONTRACT;
    return ILCodeVersionIterator(this);
}

ILCodeVersionIterator ILCodeVersionCollection::End()
{
    LIMITED_METHOD_DAC_CONTRACT;
    return ILCodeVersionIterator(NULL);
}

ILCodeVersionIterator::ILCodeVersionIterator(const ILCodeVersionIterator & iter) :
    m_stage(iter.m_stage),
    m_cur(iter.m_cur),
    m_pLinkedListCur(iter.m_pLinkedListCur),
    m_pCollection(iter.m_pCollection)
{}

ILCodeVersionIterator::ILCodeVersionIterator(ILCodeVersionCollection* pCollection) :
    m_stage(pCollection != NULL ? IterationStage::Initial : IterationStage::End),
    m_pLinkedListCur(dac_cast<PTR_ILCodeVersionNode>(nullptr)),
    m_pCollection(pCollection)
{
    LIMITED_METHOD_DAC_CONTRACT;
    First();
}

const ILCodeVersion & ILCodeVersionIterator::Get() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_cur;
}

void ILCodeVersionIterator::First()
{
    LIMITED_METHOD_DAC_CONTRACT;
    Next();
}

void ILCodeVersionIterator::Next()
{
    LIMITED_METHOD_DAC_CONTRACT;
    if (m_stage == IterationStage::Initial)
    {
        m_stage = IterationStage::ImplicitCodeVersion;
        m_cur = ILCodeVersion(m_pCollection->m_pModule, m_pCollection->m_methodDef);
        return;
    }
    if (m_stage == IterationStage::ImplicitCodeVersion)
    {
        CodeVersionManager* pCodeVersionManager = m_pCollection->m_pModule->GetCodeVersionManager();
        _ASSERTE(pCodeVersionManager->LockOwnedByCurrentThread());
        PTR_ILCodeVersioningState pILCodeVersioningState = pCodeVersionManager->GetILCodeVersioningState(m_pCollection->m_pModule, m_pCollection->m_methodDef);
        if (pILCodeVersioningState != NULL)
        {
            m_pLinkedListCur = pILCodeVersioningState->GetFirstVersionNode();
        }
        m_stage = IterationStage::LinkedList;
        if (m_pLinkedListCur != NULL)
        {
            m_cur = ILCodeVersion(m_pLinkedListCur);
            return;
        }
    }
    if (m_stage == IterationStage::LinkedList)
    {
        if (m_pLinkedListCur != NULL)
        {
            m_pLinkedListCur = m_pLinkedListCur->GetNextILVersionNode();
        }
        if (m_pLinkedListCur != NULL)
        {
            m_cur = ILCodeVersion(m_pLinkedListCur);
            return;
        }
        else
        {
            m_stage = IterationStage::End;
            m_cur = ILCodeVersion();
            return;
        }
    }
}

bool ILCodeVersionIterator::Equal(const ILCodeVersionIterator &i) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_cur == i.m_cur;
}

MethodDescVersioningState::MethodDescVersioningState(PTR_MethodDesc pMethodDesc) :
    m_pMethodDesc(pMethodDesc),
    m_flags(IsDefaultVersionActiveChildFlag),
    m_nextId(1),
    m_pFirstVersionNode(dac_cast<PTR_NativeCodeVersionNode>(nullptr))
{
    LIMITED_METHOD_DAC_CONTRACT;
}

PTR_MethodDesc MethodDescVersioningState::GetMethodDesc() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pMethodDesc;
}

#ifndef DACCESS_COMPILE
NativeCodeVersionId MethodDescVersioningState::AllocateVersionId()
{
    LIMITED_METHOD_CONTRACT;
    return m_nextId++;
}
#endif

PTR_NativeCodeVersionNode MethodDescVersioningState::GetFirstVersionNode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pFirstVersionNode;
}

BOOL MethodDescVersioningState::IsDefaultVersionActiveChild() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return (m_flags & IsDefaultVersionActiveChildFlag) != 0;
}
#ifndef DACCESS_COMPILE
void MethodDescVersioningState::SetDefaultVersionActiveChildFlag(BOOL isActive)
{
    LIMITED_METHOD_CONTRACT;
    if (isActive)
    {
        m_flags |= IsDefaultVersionActiveChildFlag;
    }
    else
    {
        m_flags &= ~IsDefaultVersionActiveChildFlag;
    }
}

void MethodDescVersioningState::LinkNativeCodeVersionNode(NativeCodeVersionNode* pNativeCodeVersionNode)
{
    LIMITED_METHOD_CONTRACT;
    pNativeCodeVersionNode->m_pNextMethodDescSibling = m_pFirstVersionNode;
    m_pFirstVersionNode = pNativeCodeVersionNode;
}
#endif

ILCodeVersioningState::ILCodeVersioningState(PTR_Module pModule, mdMethodDef methodDef) :
    m_activeVersion(ILCodeVersion(pModule,methodDef)),
    m_pFirstVersionNode(dac_cast<PTR_ILCodeVersionNode>(nullptr)),
    m_pModule(pModule),
    m_methodDef(methodDef)
{}


ILCodeVersioningState::Key::Key() :
    m_pModule(dac_cast<PTR_Module>(nullptr)),
    m_methodDef(0)
{}

ILCodeVersioningState::Key::Key(PTR_Module pModule, mdMethodDef methodDef) :
    m_pModule(pModule),
    m_methodDef(methodDef)
{}

size_t ILCodeVersioningState::Key::Hash() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return (size_t)(dac_cast<TADDR>(m_pModule) ^ m_methodDef);
}

bool ILCodeVersioningState::Key::operator==(const Key & rhs) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return (m_pModule == rhs.m_pModule) && (m_methodDef == rhs.m_methodDef);
}

ILCodeVersioningState::Key ILCodeVersioningState::GetKey() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return Key(m_pModule, m_methodDef);
}

ILCodeVersion ILCodeVersioningState::GetActiveVersion() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_activeVersion;
}

PTR_ILCodeVersionNode ILCodeVersioningState::GetFirstVersionNode() const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_pFirstVersionNode;
}

#ifndef DACCESS_COMPILE
void ILCodeVersioningState::SetActiveVersion(ILCodeVersion ilActiveCodeVersion)
{
    LIMITED_METHOD_CONTRACT;
    m_activeVersion = ilActiveCodeVersion;
}

void ILCodeVersioningState::LinkILCodeVersionNode(ILCodeVersionNode* pILCodeVersionNode)
{
    LIMITED_METHOD_CONTRACT;
    pILCodeVersionNode->SetNextILVersionNode(m_pFirstVersionNode);
    m_pFirstVersionNode = pILCodeVersionNode;
}
#endif

CodeVersionManager::CodeVersionManager()
{}

//---------------------------------------------------------------------------------------
//
// Called from BaseDomain::BaseDomain to do any constructor-time initialization.
// Presently, this takes care of initializing the Crst.
//

void CodeVersionManager::PreInit()
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        CAN_TAKE_LOCK;
        MODE_ANY;
    }
    CONTRACTL_END;

#ifndef DACCESS_COMPILE
    m_crstTable.Init(
        CrstReJITDomainTable,
        CrstFlags(CRST_UNSAFE_ANYMODE | CRST_DEBUGGER_THREAD | CRST_REENTRANCY | CRST_TAKEN_DURING_SHUTDOWN));
#endif // DACCESS_COMPILE
}

CodeVersionManager::TableLockHolder::TableLockHolder(CodeVersionManager* pCodeVersionManager) :
    CrstHolder(&pCodeVersionManager->m_crstTable)
{
}
#ifndef DACCESS_COMPILE
void CodeVersionManager::EnterLock()
{
    m_crstTable.Enter();
}
void CodeVersionManager::LeaveLock()
{
    m_crstTable.Leave();
}
#endif

#ifdef DEBUG
BOOL CodeVersionManager::LockOwnedByCurrentThread() const
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef DACCESS_COMPILE
    return TRUE;
#else
    return const_cast<CrstExplicitInit &>(m_crstTable).OwnedByCurrentThread();
#endif
}
#endif

PTR_ILCodeVersioningState CodeVersionManager::GetILCodeVersioningState(PTR_Module pModule, mdMethodDef methodDef) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    ILCodeVersioningState::Key key = ILCodeVersioningState::Key(pModule, methodDef);
    return m_ilCodeVersioningStateMap.Lookup(key);
}

PTR_MethodDescVersioningState CodeVersionManager::GetMethodDescVersioningState(PTR_MethodDesc pClosedMethodDesc) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    return m_methodDescVersioningStateMap.Lookup(pClosedMethodDesc);
}

#ifndef DACCESS_COMPILE
HRESULT CodeVersionManager::GetOrCreateILCodeVersioningState(Module* pModule, mdMethodDef methodDef, ILCodeVersioningState** ppILCodeVersioningState)
{
    LIMITED_METHOD_CONTRACT;
    HRESULT hr = S_OK;
    ILCodeVersioningState* pILCodeVersioningState = GetILCodeVersioningState(pModule, methodDef);
    if (pILCodeVersioningState == NULL)
    {
        pILCodeVersioningState = new (nothrow) ILCodeVersioningState(pModule, methodDef);
        if (pILCodeVersioningState == NULL)
        {
            return E_OUTOFMEMORY;
        }
        EX_TRY
        {
            // This throws when out of memory, but remains internally
            // consistent (without adding the new element)
            m_ilCodeVersioningStateMap.Add(pILCodeVersioningState);
        }
        EX_CATCH_HRESULT(hr);
        if (FAILED(hr))
        {
            delete pILCodeVersioningState;
            return hr;
        }
    }
    *ppILCodeVersioningState = pILCodeVersioningState;
    return S_OK;
}

HRESULT CodeVersionManager::GetOrCreateMethodDescVersioningState(MethodDesc* pMethod, MethodDescVersioningState** ppMethodVersioningState)
{
    LIMITED_METHOD_CONTRACT;
    HRESULT hr = S_OK;
    MethodDescVersioningState* pMethodVersioningState = m_methodDescVersioningStateMap.Lookup(pMethod);
    if (pMethodVersioningState == NULL)
    {
        pMethodVersioningState = new (nothrow) MethodDescVersioningState(pMethod);
        if (pMethodVersioningState == NULL)
        {
            return E_OUTOFMEMORY;
        }
        EX_TRY
        {
            // This throws when out of memory, but remains internally
            // consistent (without adding the new element)
            m_methodDescVersioningStateMap.Add(pMethodVersioningState);
        }
        EX_CATCH_HRESULT(hr);
        if (FAILED(hr))
        {
            delete pMethodVersioningState;
            return hr;
        }
    }
    *ppMethodVersioningState = pMethodVersioningState;
    return S_OK;
}
#endif // DACCESS_COMPILE

DWORD CodeVersionManager::GetNonDefaultILVersionCount()
{
    LIMITED_METHOD_DAC_CONTRACT;

    //This function is legal to call WITHOUT taking the lock
    //It is used to do a quick check if work might be needed without paying the overhead
    //of acquiring the lock and doing dictionary lookups
    return m_ilCodeVersioningStateMap.GetCount();
}

ILCodeVersionCollection CodeVersionManager::GetILCodeVersions(PTR_MethodDesc pMethod)
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return GetILCodeVersions(dac_cast<PTR_Module>(pMethod->GetModule()), pMethod->GetMemberDef());
}

ILCodeVersionCollection CodeVersionManager::GetILCodeVersions(PTR_Module pModule, mdMethodDef methodDef)
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return ILCodeVersionCollection(pModule, methodDef);
}

ILCodeVersion CodeVersionManager::GetActiveILCodeVersion(PTR_MethodDesc pMethod)
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return GetActiveILCodeVersion(dac_cast<PTR_Module>(pMethod->GetModule()), pMethod->GetMemberDef());
}

ILCodeVersion CodeVersionManager::GetActiveILCodeVersion(PTR_Module pModule, mdMethodDef methodDef)
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    ILCodeVersioningState* pILCodeVersioningState = GetILCodeVersioningState(pModule, methodDef);
    if (pILCodeVersioningState == NULL)
    {
        return ILCodeVersion(pModule, methodDef);
    }
    else
    {
        return pILCodeVersioningState->GetActiveVersion();
    }
}

ILCodeVersion CodeVersionManager::GetILCodeVersion(PTR_MethodDesc pMethod, ReJITID rejitId)
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());

#ifdef FEATURE_REJIT
    ILCodeVersionCollection collection = GetILCodeVersions(pMethod);
    for (ILCodeVersionIterator cur = collection.Begin(), end = collection.End(); cur != end; cur++)
    {
        if (cur->GetVersionId() == rejitId)
        {
            return *cur;
        }
    }
    return ILCodeVersion();
#else // FEATURE_REJIT
    _ASSERTE(rejitId == 0);
    return ILCodeVersion(dac_cast<PTR_Module>(pMethod->GetModule()), pMethod->GetMemberDef());
#endif // FEATURE_REJIT
}

NativeCodeVersionCollection CodeVersionManager::GetNativeCodeVersions(PTR_MethodDesc pMethod) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());
    return NativeCodeVersionCollection(pMethod, ILCodeVersion());
}

NativeCodeVersion CodeVersionManager::GetNativeCodeVersion(PTR_MethodDesc pMethod, PCODE codeStartAddress) const
{
    LIMITED_METHOD_DAC_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());

    NativeCodeVersionCollection nativeCodeVersions = GetNativeCodeVersions(pMethod);
    for (NativeCodeVersionIterator cur = nativeCodeVersions.Begin(), end = nativeCodeVersions.End(); cur != end; cur++)
    {
        if (cur->GetNativeCode() == codeStartAddress)
        {
            return *cur;
        }
    }
    return NativeCodeVersion();
}

#ifndef DACCESS_COMPILE
HRESULT CodeVersionManager::AddILCodeVersion(Module* pModule, mdMethodDef methodDef, ReJITID rejitId, ILCodeVersion* pILCodeVersion)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());

    ILCodeVersioningState* pILCodeVersioningState;
    HRESULT hr = GetOrCreateILCodeVersioningState(pModule, methodDef, &pILCodeVersioningState);
    if (FAILED(hr))
    {
        _ASSERTE(hr == E_OUTOFMEMORY);
        return hr;
    }

    ILCodeVersionNode* pILCodeVersionNode = new (nothrow) ILCodeVersionNode(pModule, methodDef, rejitId);
    if (pILCodeVersionNode == NULL)
    {
        return E_OUTOFMEMORY;
    }
    pILCodeVersioningState->LinkILCodeVersionNode(pILCodeVersionNode);
    *pILCodeVersion = ILCodeVersion(pILCodeVersionNode);
    return S_OK;
}

HRESULT CodeVersionManager::SetActiveILCodeVersions(ILCodeVersion* pActiveVersions, DWORD cActiveVersions, BOOL fEESuspended, CDynArray<CodePublishError> * pErrors)
{
    // If the IL version is in the shared domain we need to iterate all domains
    // looking for instantiations. The domain iterator lock is bigger than
    // the code version manager lock so we can't do this atomically. In one atomic
    // update the bookkeeping for IL versioning will happen and then in a second
    // update the active native code versions will change/precodes
    // will update.
    //
    // Note: For all domains other than the shared AppDomain we could do this
    // atomically, but for now we use the lowest common denominator for all
    // domains.
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
        CAN_TAKE_LOCK;
        PRECONDITION(CheckPointer(pActiveVersions));
        PRECONDITION(CheckPointer(pErrors, NULL_OK));
    }
    CONTRACTL_END;
    _ASSERTE(!LockOwnedByCurrentThread());
    HRESULT hr = S_OK;

#if DEBUG
    for (DWORD i = 0; i < cActiveVersions; i++)
    {
        ILCodeVersion activeVersion = pActiveVersions[i];
        if (activeVersion.IsNull())
        {
            _ASSERTE(!"The active IL version can't be NULL");
        }
    }
#endif

    // step 1 - mark the IL versions as being active, this ensures that
    // any new method instantiations added after this point will bind to
    // the correct version
    {
        TableLockHolder(this);
        for (DWORD i = 0; i < cActiveVersions; i++)
        {
            ILCodeVersion activeVersion = pActiveVersions[i];
            ILCodeVersioningState* pILCodeVersioningState = NULL;
            if (FAILED(hr = GetOrCreateILCodeVersioningState(activeVersion.GetModule(), activeVersion.GetMethodDef(), &pILCodeVersioningState)))
            {
                _ASSERTE(hr == E_OUTOFMEMORY);
                return hr;
            }
            pILCodeVersioningState->SetActiveVersion(activeVersion);
        }
    }

    // step 2 - determine the set of pre-existing method instantiations

    // a parallel array to activeVersions
    // for each ILCodeVersion in activeVersions, this lists the set
    // MethodDescs that will need to be updated
    CDynArray<CDynArray<MethodDesc*>> methodDescsToUpdate;
    CDynArray<CodePublishError> errorRecords;
    for (DWORD i = 0; i < cActiveVersions; i++)
    {
        CDynArray<MethodDesc*>* pMethodDescs = methodDescsToUpdate.Append();
        if (pMethodDescs == NULL)
        {
            return E_OUTOFMEMORY;
        }
        *pMethodDescs = CDynArray<MethodDesc*>();

        MethodDesc* pLoadedMethodDesc = pActiveVersions[i].GetModule()->LookupMethodDef(pActiveVersions[i].GetMethodDef());
        if (FAILED(hr = CodeVersionManager::EnumerateClosedMethodDescs(pLoadedMethodDesc, pMethodDescs, &errorRecords)))
        {
            _ASSERTE(hr == E_OUTOFMEMORY);
            return hr;
        }
    }

    // step 3 - update each pre-existing method instantiation
    {
        // Backpatching entry point slots requires cooperative GC mode, see
        // MethodDescBackpatchInfoTracker::Backpatch_Locked(). The code version manager's table lock is an unsafe lock that
        // may be taken in any GC mode. The lock is taken in cooperative GC mode on some other paths, so the same ordering
        // must be used here to prevent deadlock.
        GCX_COOP();
        TableLockHolder lock(this);

        for (DWORD i = 0; i < cActiveVersions; i++)
        {
            // Its possible the active IL version has changed if
            // another caller made an update while this method wasn't
            // holding the lock. We will ensure that we synchronize
            // publishing to whatever version is currently active, even
            // if that isn't the IL version we set above.
            //
            // Note: Although we attempt to handle this case gracefully
            // it isn't recommended for callers to do this. Racing two calls
            // that set the IL version to different results means it will be
            // completely arbitrary which version wins.
            ILCodeVersion requestedActiveILVersion = pActiveVersions[i];
            ILCodeVersion activeILVersion = GetActiveILCodeVersion(requestedActiveILVersion.GetModule(), requestedActiveILVersion.GetMethodDef());

            CDynArray<MethodDesc*> methodDescs = methodDescsToUpdate[i];
            for (int j = 0; j < methodDescs.Count(); j++)
            {
                // Get an the active child code version for this method instantiation (it might be NULL, that is OK)
                NativeCodeVersion activeNativeChild = activeILVersion.GetActiveNativeCodeVersion(methodDescs[j]);

                // Publish that child version, because it is the active native child of the active IL version
                // Failing to publish is non-fatal, but we do record it so the caller is aware
                if (FAILED(hr = PublishNativeCodeVersion(methodDescs[j], activeNativeChild, fEESuspended)))
                {
                    if (FAILED(hr = AddCodePublishError(activeILVersion.GetModule(), activeILVersion.GetMethodDef(), methodDescs[j], hr, &errorRecords)))
                    {
                        _ASSERTE(hr == E_OUTOFMEMORY);
                        return hr;
                    }
                }
            }
        }
    }

    return S_OK;
}

HRESULT CodeVersionManager::AddNativeCodeVersion(
    ILCodeVersion ilCodeVersion,
    MethodDesc* pClosedMethodDesc,
    NativeCodeVersion::OptimizationTier optimizationTier,
    NativeCodeVersion* pNativeCodeVersion)
{
    LIMITED_METHOD_CONTRACT;
    _ASSERTE(LockOwnedByCurrentThread());

    MethodDescVersioningState* pMethodVersioningState;
    HRESULT hr = GetOrCreateMethodDescVersioningState(pClosedMethodDesc, &pMethodVersioningState);
    if (FAILED(hr))
    {
        _ASSERTE(hr == E_OUTOFMEMORY);
        return hr;
    }

    NativeCodeVersionId newId = pMethodVersioningState->AllocateVersionId();
    NativeCodeVersionNode* pNativeCodeVersionNode = new (nothrow) NativeCodeVersionNode(newId, pClosedMethodDesc, ilCodeVersion.GetVersionId(), optimizationTier);
    if (pNativeCodeVersionNode == NULL)
    {
        return E_OUTOFMEMORY;
    }

    pMethodVersioningState->LinkNativeCodeVersionNode(pNativeCodeVersionNode);

    // the first child added is automatically considered the active one.
    if (ilCodeVersion.GetActiveNativeCodeVersion(pClosedMethodDesc).IsNull())
    {
        pNativeCodeVersionNode->SetActiveChildFlag(TRUE);
        _ASSERTE(!ilCodeVersion.GetActiveNativeCodeVersion(pClosedMethodDesc).IsNull());

        // the new child shouldn't have any native code. If it did we might need to
        // publish that code as part of adding the node which would require callers
        // to pay attention to GC suspension and we'd need to report publishing errors
        // back to them.
        _ASSERTE(pNativeCodeVersionNode->GetNativeCode() == NULL);
    }
    *pNativeCodeVersion = NativeCodeVersion(pNativeCodeVersionNode);
    return S_OK;
}

PCODE CodeVersionManager::PublishVersionableCodeIfNecessary(MethodDesc* pMethodDesc, BOOL fCanBackpatchPrestub)
{
    STANDARD_VM_CONTRACT;
    _ASSERTE(!LockOwnedByCurrentThread());
    _ASSERTE(pMethodDesc->IsVersionable());
    
    HRESULT hr = S_OK;
    PCODE pCode = NULL;

    NativeCodeVersion activeVersion;
    {
        TableLockHolder lock(this);
        if (FAILED(hr = GetActiveILCodeVersion(pMethodDesc).GetOrCreateActiveNativeCodeVersion(pMethodDesc, &activeVersion)))
        {
            _ASSERTE(hr == E_OUTOFMEMORY);
            ReportCodePublishError(pMethodDesc->GetModule(), pMethodDesc->GetMemberDef(), pMethodDesc, hr);
            return NULL;
        }
    }

    BOOL fEESuspend = FALSE;
    while (true)
    {
        // compile the code if needed
        pCode = activeVersion.GetNativeCode();
        if (pCode == NULL)
        {
            pCode = pMethodDesc->PrepareCode(activeVersion);
        }

        bool mayHaveEntryPointSlotsToBackpatch = pMethodDesc->MayHaveEntryPointSlotsToBackpatch();
        MethodDescBackpatchInfoTracker::ConditionalLockHolder lockHolder(mayHaveEntryPointSlotsToBackpatch);

        // suspend in preparation for publishing if needed
        if (fEESuspend)
        {
            ThreadSuspend::SuspendEE(ThreadSuspend::SUSPEND_FOR_REJIT);
        }

        {
            // Backpatching entry point slots requires cooperative GC mode, see
            // MethodDescBackpatchInfoTracker::Backpatch_Locked(). The code version manager's table lock is an unsafe lock that
            // may be taken in any GC mode. The lock is taken in cooperative GC mode on some other paths, so the same ordering
            // must be used here to prevent deadlock.
            GCX_MAYBE_COOP(mayHaveEntryPointSlotsToBackpatch);
            TableLockHolder lock(this);

            // The common case is that newActiveCode == activeCode, however we did leave the lock so there is
            // possibility that the active version has changed. If it has we need to restart the compilation
            // and publishing process with the new active version instead.
            //
            // In theory it should be legitimate to break out of this loop and run the less recent active version,
            // because ultimately this is a race between one thread that is updating the version and another thread
            // trying to run the current version. However for back-compat with ReJIT we need to guarantee that
            // a versioning update at least as late as the profiler JitCompilationFinished callback wins the race.
            NativeCodeVersion newActiveVersion;
            if (FAILED(hr = GetActiveILCodeVersion(pMethodDesc).GetOrCreateActiveNativeCodeVersion(pMethodDesc, &newActiveVersion)))
            {
                _ASSERTE(hr == E_OUTOFMEMORY);
                ReportCodePublishError(pMethodDesc->GetModule(), pMethodDesc->GetMemberDef(), pMethodDesc, hr);
                pCode = NULL;
                break;
            }
            if (newActiveVersion != activeVersion)
            {
                activeVersion = newActiveVersion;
            }
            else
            {
                // if we aren't allowed to backpatch we are done
                if (!fCanBackpatchPrestub)
                {
                    break;
                }

                // attempt to publish the active version still under the lock
                if (FAILED(hr = PublishNativeCodeVersion(pMethodDesc, activeVersion, fEESuspend)))
                {
                    // If we need an EESuspend to publish then start over. We have to leave the lock in order to suspend,
                    // and when we leave the lock the active version might change again. However now we know that suspend is
                    // necessary.
                    if (hr == CORPROF_E_RUNTIME_SUSPEND_REQUIRED)
                    {
                        _ASSERTE(!fEESuspend);
                        fEESuspend = true;
                        continue; // skip RestartEE() below since SuspendEE() has not been called yet
                    }
                    else
                    {
                        ReportCodePublishError(pMethodDesc->GetModule(), pMethodDesc->GetMemberDef(), pMethodDesc, hr);
                        pCode = NULL;
                        break;
                    }
                }
                else
                {
                    //success
                    break;
                }
            }
        } // exit lock, revert GC mode

        if (fEESuspend)
        {
            ThreadSuspend::RestartEE(FALSE, TRUE);
        }
    }
    
    // if the EE is still suspended from breaking in the middle of the loop, resume it
    if (fEESuspend)
    {
        ThreadSuspend::RestartEE(FALSE, TRUE);
    }
    return pCode;
}

HRESULT CodeVersionManager::PublishNativeCodeVersion(MethodDesc* pMethod, NativeCodeVersion nativeCodeVersion, BOOL fEESuspended)
{
    // TODO: This function needs to make sure it does not change the precode's target if call counting is in progress. Track
    // whether call counting is currently being done for the method, and use a lock to ensure the expected precode target.

    CONTRACTL
    {
        GC_NOTRIGGER;

        // Backpatching entry point slots requires cooperative GC mode, see MethodDescBackpatchInfoTracker::Backpatch_Locked().
        // The code version manager's table lock is an unsafe lock that may be taken in any GC mode. The lock is taken in
        // cooperative GC mode on other paths, so the caller must use the same ordering to prevent deadlock (switch to
        // cooperative GC mode before taking the lock).
        if (pMethod->MayHaveEntryPointSlotsToBackpatch())
        {
            MODE_COOPERATIVE;
        }
        else
        {
            MODE_ANY;
        }
        NOTHROW;
    }
    CONTRACTL_END;

    _ASSERTE(LockOwnedByCurrentThread());
    _ASSERTE(pMethod->IsVersionable());

    HRESULT hr = S_OK;
    PCODE pCode = nativeCodeVersion.IsNull() ? NULL : nativeCodeVersion.GetNativeCode();
    if (pMethod->IsVersionable())
    {
        EX_TRY
        {
            if (pCode == NULL)
            {
                pMethod->ResetCodeEntryPoint();
            }
            else
            {
                pMethod->SetCodeEntryPoint(pCode);
            }
        }
        EX_CATCH_HRESULT(hr);
        return hr;
    }
    else
    {
        _ASSERTE(!"This method doesn't support versioning but was requested to be versioned.");
        return E_FAIL;
    }
}

// static
HRESULT CodeVersionManager::EnumerateClosedMethodDescs(
    MethodDesc* pMD,
    CDynArray<MethodDesc*> * pClosedMethodDescs,
    CDynArray<CodePublishError> * pUnsupportedMethodErrors)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        MODE_PREEMPTIVE;
        CAN_TAKE_LOCK;
        PRECONDITION(CheckPointer(pMD, NULL_OK));
        PRECONDITION(CheckPointer(pClosedMethodDescs));
        PRECONDITION(CheckPointer(pUnsupportedMethodErrors));
    }
    CONTRACTL_END;
    HRESULT hr = S_OK;
    if (pMD == NULL)
    {
        // nothing is loaded yet so we're done for this method.
        return S_OK;
    }

    if (!pMD->HasClassOrMethodInstantiation())
    {
        // We have a JITted non-generic.
        MethodDesc ** ppMD = pClosedMethodDescs->Append();
        if (ppMD == NULL)
        {
            return E_OUTOFMEMORY;
        }
        *ppMD = pMD;
    }

    if (!pMD->HasClassOrMethodInstantiation())
    {
        // not generic, we're done for this method
        return S_OK;
    }

    // Ok, now the case of a generic function (or function on generic class), which
    // is loaded, and may thus have compiled instantiations.
    // It's impossible to get to any other kind of domain from the profiling API
    Module* pModule = pMD->GetModule();
    mdMethodDef methodDef = pMD->GetMemberDef();
    BaseDomain * pBaseDomainFromModule = pModule->GetDomain();
    _ASSERTE(pBaseDomainFromModule->IsAppDomain() ||
        pBaseDomainFromModule->IsSharedDomain());

    if (pBaseDomainFromModule->IsSharedDomain())
    {
        // Iterate through all modules loaded into the shared domain, to
        // find all instantiations living in the shared domain. This will
        // include orphaned code (i.e., shared code used by ADs that have
        // all unloaded), which is good, because orphaned code could get
        // re-adopted if a new AD is created that can use that shared code
        hr = EnumerateDomainClosedMethodDescs(
            NULL,  // NULL means to search SharedDomain instead of an AD
            pModule,
            methodDef,
            pClosedMethodDescs,
            pUnsupportedMethodErrors);
    }
    else
    {
        // Module is unshared, so just use the module's domain to find instantiations.
        hr = EnumerateDomainClosedMethodDescs(
            pBaseDomainFromModule->AsAppDomain(),
            pModule,
            methodDef,
            pClosedMethodDescs,
            pUnsupportedMethodErrors);
    }
    if (FAILED(hr))
    {
        _ASSERTE(hr == E_OUTOFMEMORY);
        return hr;
    }

    // We want to iterate through all compilations of existing instantiations to
    // ensure they get marked for rejit.  Note: There may be zero instantiations,
    // but we won't know until we try.
    if (pBaseDomainFromModule->IsSharedDomain())
    {
        // Iterate through all real domains, to find shared instantiations.
        AppDomainIterator appDomainIterator(TRUE);
        while (appDomainIterator.Next())
        {
            AppDomain * pAppDomain = appDomainIterator.GetDomain();
            hr = EnumerateDomainClosedMethodDescs(
                pAppDomain,
                pModule,
                methodDef,
                pClosedMethodDescs,
                pUnsupportedMethodErrors);
            if (FAILED(hr))
            {
                _ASSERTE(hr == E_OUTOFMEMORY);
                return hr;
            }
        }
    }
    return S_OK;
}

// static
HRESULT CodeVersionManager::EnumerateDomainClosedMethodDescs(
    AppDomain * pAppDomainToSearch,
    Module* pModuleContainingMethodDef,
    mdMethodDef methodDef,
    CDynArray<MethodDesc*> * pClosedMethodDescs,
    CDynArray<CodePublishError> * pUnsupportedMethodErrors)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_PREEMPTIVE;
        CAN_TAKE_LOCK;
        PRECONDITION(CheckPointer(pAppDomainToSearch, NULL_OK));
        PRECONDITION(CheckPointer(pModuleContainingMethodDef));
        PRECONDITION(CheckPointer(pClosedMethodDescs));
        PRECONDITION(CheckPointer(pUnsupportedMethodErrors));
    }
    CONTRACTL_END;

    _ASSERTE(methodDef != mdTokenNil);

    HRESULT hr;

    BaseDomain * pDomainContainingGenericDefinition = pModuleContainingMethodDef->GetDomain();

#ifdef _DEBUG
    // If the generic definition is not loaded domain-neutral, then all its
    // instantiations will also be non-domain-neutral and loaded into the same
    // domain as the generic definition.  So the caller may only pass the
    // domain containing the generic definition as pAppDomainToSearch
    if (!pDomainContainingGenericDefinition->IsSharedDomain())
    {
        _ASSERTE(pDomainContainingGenericDefinition == pAppDomainToSearch);
    }
#endif //_DEBUG

    // these are the default flags which won't actually be used in shared mode other than
    // asserting they were specified with their default values
    AssemblyIterationFlags assemFlags = (AssemblyIterationFlags)(kIncludeLoaded | kIncludeExecution);
    ModuleIterationOption moduleFlags = (ModuleIterationOption)kModIterIncludeLoaded;
    if (pAppDomainToSearch != NULL)
    {
        assemFlags = (AssemblyIterationFlags)(kIncludeAvailableToProfilers | kIncludeExecution);
        moduleFlags = (ModuleIterationOption)kModIterIncludeAvailableToProfilers;
    }
    LoadedMethodDescIterator it(
        pAppDomainToSearch,
        pModuleContainingMethodDef,
        methodDef,
        assemFlags,
        moduleFlags);
    CollectibleAssemblyHolder<DomainAssembly *> pDomainAssembly;
    while (it.Next(pDomainAssembly.This()))
    {
        MethodDesc * pLoadedMD = it.Current();

        if (!pLoadedMD->IsVersionable())
        {
            // For compatibility with the rejit APIs we ensure certain errors are detected and reported using their
            // original HRESULTS
            HRESULT errorHR = GetNonVersionableError(pLoadedMD);
            if (FAILED(errorHR))
            {
                if (FAILED(hr = CodeVersionManager::AddCodePublishError(pModuleContainingMethodDef, methodDef, pLoadedMD, CORPROF_E_FUNCTION_IS_COLLECTIBLE, pUnsupportedMethodErrors)))
                {
                    _ASSERTE(hr == E_OUTOFMEMORY);
                    return hr;
                }
            }
            continue;
        }
        
#ifdef _DEBUG
        if (!pDomainContainingGenericDefinition->IsSharedDomain())
        {
            // Method is defined outside of the shared domain, so its instantiation must
            // be defined in the AD we're iterating over (pAppDomainToSearch, which, as
            // asserted above, must be the same domain as the generic's definition)
            _ASSERTE(pLoadedMD->GetDomain() == pAppDomainToSearch);
        }
#endif // _DEBUG

        MethodDesc ** ppMD = pClosedMethodDescs->Append();
        if (ppMD == NULL)
        {
            return E_OUTOFMEMORY;
        }
        *ppMD = pLoadedMD;
    }
    return S_OK;
}
#endif // DACCESS_COMPILE

#ifndef DACCESS_COMPILE
//static
void CodeVersionManager::OnAppDomainExit(AppDomain * pAppDomain)
{
    LIMITED_METHOD_CONTRACT;
    // This would clean up all the allocations we have done and synchronize with any threads that might
    // still be using the data
    _ASSERTE(!".NET Core shouldn't be doing app domain shutdown - if we start doing so this needs to be implemented");
}
#endif

// Returns true if CodeVersionManager is capable of versioning this method. There may be other reasons that the runtime elects
// not to version a method even if CodeVersionManager could support it. Use the MethodDesc::IsVersionableWith*() accessors to
// get the final determination of versioning support for a given method.
//
//static
bool CodeVersionManager::IsMethodSupported(PTR_MethodDesc pMethodDesc)
{
    WRAPPER_NO_CONTRACT;
    _ASSERTE(pMethodDesc != NULL);

    return
        // CodeVersionManager data structures don't properly handle the lifetime semantics of dynamic code at this point
        !pMethodDesc->IsDynamicMethod() &&

        // CodeVersionManager data structures don't properly handle the lifetime semantics of collectible code at this point
        !pMethodDesc->GetLoaderAllocator()->IsCollectible() &&

        // EnC has its own way of versioning
        !pMethodDesc->IsEnCMethod();
}

//---------------------------------------------------------------------------------------
//
// Small helper to determine whether a given (possibly instantiated generic) MethodDesc
// is safe to rejit.
//
// Arguments:
//      pMD - MethodDesc to test
// Return Value:
//      S_OK iff pMD is safe to rejit
//      CORPROF_E_FUNCTION_IS_COLLECTIBLE - function can't be rejitted because it is collectible
//      

// static
#ifndef DACCESS_COMPILE
HRESULT CodeVersionManager::GetNonVersionableError(MethodDesc* pMD)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        CAN_TAKE_LOCK;
        MODE_ANY;
    }
    CONTRACTL_END;

    _ASSERTE(pMD != NULL);

    // Weird, non-user functions were already weeded out in RequestReJIT(), and will
    // also never be passed to us by the prestub worker (for the pre-rejit case).
    _ASSERTE(pMD->IsIL());

    // Any MethodDescs that could be collected are not currently supported.  Although we
    // rule out all Ref.Emit modules in RequestReJIT(), there can still exist types defined
    // in a non-reflection module and instantiated into a collectible assembly
    // (e.g., List<MyCollectibleStruct>).  In the future we may lift this
    // restriction by updating the ReJitManager when the collectible assemblies
    // owning the instantiations get collected.
    if (pMD->GetLoaderAllocator()->IsCollectible())
    {
        return CORPROF_E_FUNCTION_IS_COLLECTIBLE;
    }

    return S_OK;
}
#endif

//---------------------------------------------------------------------------------------
//
// Helper that inits a new CodePublishError and adds it to the pErrors array
//
// Arguments:
//      * pModule - The module in the module/MethodDef identifier pair for the method which
//                  had an error during rejit
//      * methodDef - The MethodDef in the module/MethodDef identifier pair for the method which
//                  had an error during rejit
//      * pMD - If available, the specific method instance which had an error during rejit
//      * hrStatus - HRESULT for the rejit error that occurred
//      * pErrors - the list of error records that this method will append to
//
// Return Value:
//      * S_OK: error was appended
//      * E_OUTOFMEMORY: Not enough memory to create the new error item. The array is unchanged.
//

//static
#ifndef DACCESS_COMPILE
HRESULT CodeVersionManager::AddCodePublishError(Module* pModule, mdMethodDef methodDef, MethodDesc* pMD, HRESULT hrStatus, CDynArray<CodePublishError> * pErrors)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    if (pErrors == NULL)
    {
        return S_OK;
    }

    CodePublishError* pError = pErrors->Append();
    if (pError == NULL)
    {
        return E_OUTOFMEMORY;
    }
    pError->pModule = pModule;
    pError->methodDef = methodDef;
    pError->pMethodDesc = pMD;
    pError->hrStatus = hrStatus;
    return S_OK;
}
#endif

#ifndef DACCESS_COMPILE
void CodeVersionManager::ReportCodePublishError(CodePublishError* pErrorRecord)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        CAN_TAKE_LOCK;
        MODE_ANY;
    }
    CONTRACTL_END;

    ReportCodePublishError(pErrorRecord->pModule, pErrorRecord->methodDef, pErrorRecord->pMethodDesc, pErrorRecord->hrStatus);
}

void CodeVersionManager::ReportCodePublishError(Module* pModule, mdMethodDef methodDef, MethodDesc* pMD, HRESULT hrStatus)
{
    CONTRACTL
    {
        NOTHROW;
        GC_TRIGGERS;
        CAN_TAKE_LOCK;
        MODE_ANY;
    }
    CONTRACTL_END;

#ifdef FEATURE_REJIT
    BOOL isRejitted = FALSE;
    {
        TableLockHolder(this);
        isRejitted = !GetActiveILCodeVersion(pModule, methodDef).IsDefaultVersion();
    }

    // this isn't perfect, we might be activating a tiered jitting variation of a rejitted
    // method for example. If it proves to be an issue we can revisit.
    if (isRejitted)
    {
        ReJitManager::ReportReJITError(pModule, methodDef, pMD, hrStatus);
    }
#endif
}
#endif // DACCESS_COMPILE

#endif // FEATURE_CODE_VERSIONING

