// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full
// license information.

#include "cor_profiler.h"
#include <fstream>
#include <string>
#include <vector>
#include "ComPtr.h"
#include "ILRewriter.h"
#include "Macros.h"
#include "ModuleMetadata.h"
#include "clr_helpers.h"
#include "integration_loader.h"
#include "metadata_builder.h"
#include "util.h"

namespace trace {

CorProfiler* profiler = nullptr;

CorProfiler::CorProfiler()
    : integrations_(trace::LoadIntegrationsFromEnvironment()) {}

HRESULT STDMETHODCALLTYPE
CorProfiler::Initialize(IUnknown* pICorProfilerInfoUnk) {
  is_attached_ = FALSE;

  auto process_name = GetCurrentProcessName();
  auto process_names = GetEnvironmentValues(kProcessesEnvironmentName);

  if (process_names.size() == 0) {
    LOG_APPEND(
        L"DATADOG_PROFILER_PROCESSES environment variable not set. Attaching "
        L"to any .NET process.");
  } else {
    LOG_APPEND(L"DATADOG_PROFILER_PROCESSES:");
    for (auto& name : process_names) {
      LOG_APPEND(L"  " + name);
    }

    if (std::find(process_names.begin(), process_names.end(), process_name) ==
        process_names.end()) {
      LOG_APPEND(L"CorProfiler disabled: module name \""
                 << process_name
                 << "\" does not match DATADOG_PROFILER_PROCESSES environment "
                    "variable.");
      return E_FAIL;
    }
  }

  HRESULT hr =
      pICorProfilerInfoUnk->QueryInterface<ICorProfilerInfo3>(&this->info_);
  LOG_IFFAILEDRET(hr,
                  L"CorProfiler disabled: interface ICorProfilerInfo3 or "
                  L"higher not found.");

  const DWORD eventMask =
      COR_PRF_MONITOR_JIT_COMPILATION |
      COR_PRF_DISABLE_TRANSPARENCY_CHECKS_UNDER_FULL_TRUST | /* helps the case
                                                                where this
                                                                profiler is used
                                                                on Full CLR */
      // COR_PRF_DISABLE_INLINING |
      COR_PRF_MONITOR_MODULE_LOADS |
      // COR_PRF_MONITOR_ASSEMBLY_LOADS |
      // COR_PRF_MONITOR_APPDOMAIN_LOADS |
      // COR_PRF_ENABLE_REJIT |
      COR_PRF_DISABLE_ALL_NGEN_IMAGES;

  hr = this->info_->SetEventMask(eventMask);
  LOG_IFFAILEDRET(hr, L"Failed to attach profiler: unable to set event mask.");

  // we're in!
  LOG_APPEND(L"CorProfiler attached to process " << process_name);
  this->info_->AddRef();
  is_attached_ = true;
  profiler = this;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleLoadFinished(ModuleID moduleId,
                                                          HRESULT hrStatus) {
  LPCBYTE pbBaseLoadAddr;
  WCHAR wszModulePath[MAX_PATH];
  ULONG cchNameOut;
  AssemblyID assembly_id = 0;
  DWORD dwModuleFlags;

  HRESULT hr = this->info_->GetModuleInfo2(
      moduleId, &pbBaseLoadAddr, _countof(wszModulePath), &cchNameOut,
      wszModulePath, &assembly_id, &dwModuleFlags);

  LOG_IFFAILEDRET(hr,
                  L"GetModuleInfo2 failed for ModuleID = " << HEX(moduleId));

  if ((dwModuleFlags & COR_PRF_MODULE_WINDOWS_RUNTIME) != 0) {
    // Ignore any Windows Runtime modules.  We cannot obtain writeable metadata
    // interfaces on them or instrument their IL
    return S_OK;
  }

  auto assembly_name = GetAssemblyName(this->info_, assembly_id);

  std::vector<integration> enabledIntegrations;

  // check if we need to instrument anything in this assembly,
  // for each integration...
  for (const auto& integration : this->integrations_) {
    // TODO: check if integration is enabled in config
    for (const auto& method_replacement : integration.method_replacements) {
      if (method_replacement.caller_method.assembly.name.empty() ||
          method_replacement.caller_method.assembly.name == assembly_name) {
        enabledIntegrations.push_back(integration);
      }
    }
  }

  LOG_APPEND(L"ModuleLoadFinished for "
             << assembly_name << ". Emitting instrumentation metadata.");

  if (enabledIntegrations.empty()) {
    // we don't need to instrument anything in this module, skip it
    return S_OK;
  }

  ComPtr<IUnknown> metadataInterfaces;

  hr = this->info_->GetModuleMetaData(moduleId, ofRead | ofWrite,
                                      IID_IMetaDataImport,
                                      metadataInterfaces.GetAddressOf());

  LOG_IFFAILEDRET(hr, L"Failed to get metadata interface.");

  const auto metadataImport =
      metadataInterfaces.As<IMetaDataImport>(IID_IMetaDataImport);
  const auto metadataEmit =
      metadataInterfaces.As<IMetaDataEmit>(IID_IMetaDataEmit);
  const auto assemblyImport = metadataInterfaces.As<IMetaDataAssemblyImport>(
      IID_IMetaDataAssemblyImport);
  const auto assemblyEmit =
      metadataInterfaces.As<IMetaDataAssemblyEmit>(IID_IMetaDataAssemblyEmit);

  mdModule module;
  hr = metadataImport->GetModuleFromScope(&module);
  LOG_IFFAILEDRET(hr, L"Failed to get module token.");

  ModuleMetadata* moduleMetadata =
      new ModuleMetadata(metadataImport, assembly_name, enabledIntegrations);

  trace::MetadataBuilder metadataBuilder(*moduleMetadata, module,
                                         metadataImport, metadataEmit,
                                         assemblyImport, assemblyEmit);

  for (const auto& integration : enabledIntegrations) {
    for (const auto& method_replacement : integration.method_replacements) {
      // for each wrapper assembly, emit an assembly reference
      hr = metadataBuilder.EmitAssemblyRef(
          method_replacement.wrapper_method.assembly);
      RETURN_OK_IF_FAILED(hr);

      // for each method replacement in each enabled integration,
      // emit a reference to the instrumentation wrapper methods
      hr = metadataBuilder.StoreWrapperMethodRef(method_replacement);
      RETURN_OK_IF_FAILED(hr);
    }
  }

  // store module info for later lookup
  module_id_to_info_map_.Update(moduleId, moduleMetadata);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleUnloadFinished(ModuleID moduleId,
                                                            HRESULT hrStatus) {
  ModuleMetadata* metadata;

  if (module_id_to_info_map_.LookupIfExists(moduleId, &metadata)) {
    module_id_to_info_map_.Erase(moduleId);
    delete metadata;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE
CorProfiler::JITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock) {
  ClassID classId;
  ModuleID moduleId;
  mdToken functionToken = mdTokenNil;

  HRESULT hr = this->info_->GetFunctionInfo(functionId, &classId, &moduleId,
                                            &functionToken);
  RETURN_OK_IF_FAILED(hr);

  ModuleMetadata* moduleMetadata = nullptr;

  if (!module_id_to_info_map_.LookupIfExists(moduleId, &moduleMetadata)) {
    // we haven't stored a ModuleInfo for this module, so we can't modify its IL
    return S_OK;
  }

  // get function info
  auto caller =
      GetFunctionInfo(moduleMetadata->metadata_import, functionToken);
  if (!caller.isvalid()) {
    return S_OK;
  }

  const int string_size = 1024;

  // check if we need to replace any methods called from this method
  for (const auto& integration : moduleMetadata->integrations) {
    for (const auto& method_replacement : integration.method_replacements) {
      // check known callers for IL opcodes that call into the target method.
      // if found, replace with calls to the instrumentation wrapper
      // (wrapper_method_ref)
      if ((method_replacement.caller_method.type_name.empty() ||
           method_replacement.caller_method.type_name == caller.type.name) &&
          (method_replacement.caller_method.method_name.empty() ||
           method_replacement.caller_method.method_name ==
               caller.name)) {
        const auto& wrapper_method_key =
            method_replacement.wrapper_method.get_method_cache_key();
        mdMemberRef wrapper_method_ref = mdMemberRefNil;

        if (!moduleMetadata->TryGetWrapperMemberRef(wrapper_method_key,
                                                    wrapper_method_ref)) {
          // no method ref token found for wrapper method, we can't do the
          // replacement, this should never happen because we always try to add
          // the method ref in ModuleLoadFinished()
          // TODO: log this
          return S_OK;
        }

        ILRewriter rewriter(this->info_, nullptr, moduleId, functionToken);

        // hr = rewriter.Initialize();
        hr = rewriter.Import();
        RETURN_OK_IF_FAILED(hr);

        bool modified = false;

        // for each IL instruction
        for (ILInstr* pInstr = rewriter.GetILList()->m_pNext;
             pInstr != rewriter.GetILList(); pInstr = pInstr->m_pNext) {
          // if its opcode is CALL or CALLVIRT
          if ((pInstr->m_opcode == CEE_CALL ||
               pInstr->m_opcode == CEE_CALLVIRT) &&
              (TypeFromToken(pInstr->m_Arg32) == mdtMemberRef ||
               TypeFromToken(pInstr->m_Arg32) == mdtMethodDef)) {
            WCHAR target_method_name[string_size]{};
            ULONG target_method_name_length = 0;

            WCHAR target_type_name[string_size]{};
            ULONG target_type_name_length = 0;

            mdMethodDef target_method_def = mdMethodDefNil;
            mdTypeDef target_type_def = mdTypeDefNil;

            if (TypeFromToken(pInstr->m_Arg32) == mdtMemberRef) {
              // get function name from mdMemberRef
              mdToken token = mdTokenNil;
              hr = moduleMetadata->metadata_import->GetMemberRefProps(
                  pInstr->m_Arg32, &token, target_method_name, string_size,
                  &target_method_name_length, nullptr, nullptr);
              RETURN_OK_IF_FAILED(hr);

              if (method_replacement.target_method.method_name !=
                  target_method_name) {
                // method name doesn't match, skip to next instruction
                continue;
              }

              // determine how to get type name from token, depending on the
              // token type
              if (TypeFromToken(token) == mdtTypeRef) {
                hr = moduleMetadata->metadata_import->GetTypeRefProps(
                    token, nullptr, target_type_name, string_size,
                    &target_type_name_length);
                RETURN_OK_IF_FAILED(hr);
                goto compare_type_and_method_names;
              }

              if (TypeFromToken(token) == mdtTypeDef) {
                target_type_def = token;
                goto use_type_def;
              }

              if (TypeFromToken(token) == mdtMethodDef) {
                // we got an mdMethodDef back, so jump to where we use a
                // methodDef instead of a methodRef
                target_method_def = token;
                goto use_method_def;
              }

              // value of token is not a supported token type, skip to next
              // instruction
              continue;
            }

            // if pInstr->m_Arg32 wasn't an mdtMemberRef, it must be an
            // mdtMethodDef
            target_method_def = pInstr->m_Arg32;

          use_method_def:
            // get function name from mdMethodDef
            hr = moduleMetadata->metadata_import->GetMethodProps(
                target_method_def, &target_type_def, target_method_name,
                string_size, &target_method_name_length, nullptr, nullptr,
                nullptr, nullptr, nullptr);
            RETURN_OK_IF_FAILED(hr);

            if (method_replacement.target_method.method_name !=
                target_method_name) {
              // method name doesn't match, skip to next instruction
              continue;
            }

          use_type_def:
            // get type name from mdTypeDef
            hr = moduleMetadata->metadata_import->GetTypeDefProps(
                target_type_def, target_type_name, string_size,
                &target_type_name_length, nullptr, nullptr);
            RETURN_OK_IF_FAILED(hr);

          compare_type_and_method_names:
            // if the target matches by type name and method name
            if (method_replacement.target_method.type_name ==
                    target_type_name &&
                method_replacement.target_method.method_name ==
                    target_method_name) {
              // replace with a call to the instrumentation wrapper
              pInstr->m_opcode = CEE_CALL;
              pInstr->m_Arg32 = wrapper_method_ref;

              modified = true;
            }
          }
        }

        if (modified) {
          hr = rewriter.Export();
          return S_OK;
        }
      }
    }
  }

  return S_OK;
}

bool CorProfiler::IsAttached() const { return is_attached_; }

}  // namespace trace
