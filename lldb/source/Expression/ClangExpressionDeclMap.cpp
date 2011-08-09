//===-- ClangExpressionDeclMap.cpp -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/ClangExpressionDeclMap.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Decl.h"
#include "lldb/lldb-private.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/Expression/ASTDumper.h"
#include "lldb/Expression/ClangASTSource.h"
#include "lldb/Expression/ClangPersistentVariables.h"
#include "lldb/Host/Endian.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ClangNamespaceDecl.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb;
using namespace lldb_private;
using namespace clang;

ClangExpressionDeclMap::ClangExpressionDeclMap (bool keep_result_in_memory) :
    m_found_entities (),
    m_struct_members (),
    m_keep_result_in_memory (keep_result_in_memory),
    m_parser_vars (),
    m_struct_vars ()
{
    EnableStructVars();
}

ClangExpressionDeclMap::~ClangExpressionDeclMap()
{
    DidDematerialize();
    DisableStructVars();
}

bool 
ClangExpressionDeclMap::WillParse(ExecutionContext &exe_ctx)
{    
    EnableParserVars();
    m_parser_vars->m_exe_ctx = &exe_ctx;
        
    if (exe_ctx.frame)
        m_parser_vars->m_sym_ctx = exe_ctx.frame->GetSymbolContext(lldb::eSymbolContextEverything);
    else if (exe_ctx.thread)
        m_parser_vars->m_sym_ctx = exe_ctx.thread->GetStackFrameAtIndex(0)->GetSymbolContext(lldb::eSymbolContextEverything);
    else if (exe_ctx.process)
        m_parser_vars->m_sym_ctx = SymbolContext(exe_ctx.target->GetSP(), ModuleSP());
    if (exe_ctx.target)
        m_parser_vars->m_persistent_vars = &exe_ctx.target->GetPersistentVariables();
    
    if (exe_ctx.target && !exe_ctx.target->GetScratchClangASTContext())
        return false;
    
    return true;
}

void 
ClangExpressionDeclMap::DidParse()
{
    if (m_parser_vars.get())
    {
        for (size_t entity_index = 0, num_entities = m_found_entities.GetSize();
             entity_index < num_entities;
             ++entity_index)
        {
            ClangExpressionVariableSP var_sp(m_found_entities.GetVariableAtIndex(entity_index));
            if (var_sp && 
                var_sp->m_parser_vars.get() && 
                var_sp->m_parser_vars->m_lldb_value)
                delete var_sp->m_parser_vars->m_lldb_value;
            
            var_sp->DisableParserVars();
        }
        
        for (size_t pvar_index = 0, num_pvars = m_parser_vars->m_persistent_vars->GetSize();
             pvar_index < num_pvars;
             ++pvar_index)
        {
            ClangExpressionVariableSP pvar_sp(m_parser_vars->m_persistent_vars->GetVariableAtIndex(pvar_index));
            if (pvar_sp)
                pvar_sp->DisableParserVars();
        }
        
        DisableParserVars();
    }
}

// Interface for IRForTarget

const ConstString &
ClangExpressionDeclMap::GetPersistentResultName ()
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    if (!m_struct_vars->m_result_name)
    {
        Target *target = m_parser_vars->GetTarget();
        assert (target);
        m_struct_vars->m_result_name = target->GetPersistentVariables().GetNextPersistentVariableName();
    }
    return m_struct_vars->m_result_name;
}

lldb::ClangExpressionVariableSP
ClangExpressionDeclMap::BuildIntegerVariable (const ConstString &name,
                                              lldb_private::TypeFromParser type,
                                              const llvm::APInt& value)
{
    assert (m_parser_vars.get());
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    ASTContext *context(exe_ctx->target->GetScratchClangASTContext()->getASTContext());
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     type.GetASTContext(),
                                                     type.GetOpaqueQualType()),
                           context);
        
    if (!m_parser_vars->m_persistent_vars->CreatePersistentVariable (exe_ctx->GetBestExecutionContextScope (),
                                                                     name, 
                                                                     user_type, 
                                                                     exe_ctx->process->GetByteOrder(),
                                                                     exe_ctx->process->GetAddressByteSize()))
        return lldb::ClangExpressionVariableSP();
    
    ClangExpressionVariableSP pvar_sp (m_parser_vars->m_persistent_vars->GetVariable(name));
    
    if (!pvar_sp)
        return lldb::ClangExpressionVariableSP();
    
    uint8_t *pvar_data = pvar_sp->GetValueBytes();
    if (pvar_data == NULL)
        return lldb::ClangExpressionVariableSP();
    
    uint64_t value64 = value.getLimitedValue();
    
    ByteOrder byte_order = exe_ctx->process->GetByteOrder();
    
    size_t num_val_bytes = sizeof(value64);
    size_t num_data_bytes = pvar_sp->GetByteSize();
    
    size_t num_bytes = num_val_bytes;
    if (num_bytes > num_data_bytes)
        num_bytes = num_data_bytes;
    
    for (size_t byte_idx = 0;
         byte_idx < num_bytes;
         ++byte_idx)
    {
        uint64_t shift = byte_idx * 8;
        uint64_t mask = 0xffll << shift;
        uint8_t cur_byte = (uint8_t)((value64 & mask) >> shift);
        
        switch (byte_order)
        {
            case eByteOrderBig:
                //                    High         Low
                // Original:         |AABBCCDDEEFFGGHH|
                // Target:                   |EEFFGGHH|
                
                pvar_data[num_data_bytes - (1 + byte_idx)] = cur_byte;
                break;
            case eByteOrderLittle:
                // Target:                   |HHGGFFEE|
                pvar_data[byte_idx] = cur_byte;
                break;
            default:
                return lldb::ClangExpressionVariableSP();    
        }
    }
    
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsFreezeDried;
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
    pvar_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;

    return pvar_sp;
}

lldb::ClangExpressionVariableSP
ClangExpressionDeclMap::BuildCastVariable (const ConstString &name,
                                           VarDecl *decl,
                                           lldb_private::TypeFromParser type)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    ASTContext *context(exe_ctx->target->GetScratchClangASTContext()->getASTContext());
    
    ClangExpressionVariableSP var_sp (m_found_entities.GetVariable(decl));
    
    if (!var_sp)
        var_sp = m_parser_vars->m_persistent_vars->GetVariable(decl);
    
    if (!var_sp)
        return ClangExpressionVariableSP();
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     type.GetASTContext(),
                                                     type.GetOpaqueQualType()),
                           context);
    
    TypeFromUser var_type = var_sp->GetTypeFromUser();
    
    VariableSP var = FindVariableInScope (*exe_ctx->frame, var_sp->GetName(), &var_type);
    
    if (!var)
        return lldb::ClangExpressionVariableSP(); // but we should handle this; it may be a persistent variable
    
    ValueObjectSP var_valobj = exe_ctx->frame->GetValueObjectForFrameVariable(var, lldb::eNoDynamicValues);

    if (!var_valobj)
        return lldb::ClangExpressionVariableSP();
    
    ValueObjectSP var_casted_valobj = var_valobj->CastPointerType(name.GetCString(), user_type);
    
    if (!var_casted_valobj)
        return lldb::ClangExpressionVariableSP();
    
    if (log)
    {
        StreamString my_stream_string;
        
        ClangASTType::DumpTypeDescription (var_type.GetASTContext(),
                                           var_type.GetOpaqueQualType(),
                                           &my_stream_string);
        
        
        log->Printf("Building cast variable to type: %s", my_stream_string.GetString().c_str());
    }
    
    ClangExpressionVariableSP pvar_sp = m_parser_vars->m_persistent_vars->CreatePersistentVariable (var_casted_valobj);
    
    if (!pvar_sp)
        return lldb::ClangExpressionVariableSP();
    
    if (pvar_sp != m_parser_vars->m_persistent_vars->GetVariable(name))
        return lldb::ClangExpressionVariableSP();
    
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsFreezeDried;
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
    pvar_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
            
    return pvar_sp;
}

bool 
ClangExpressionDeclMap::AddPersistentVariable 
(
    const NamedDecl *decl, 
    const ConstString &name, 
    TypeFromParser parser_type,
    bool is_result,
    bool is_lvalue
)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    
    ASTContext *context(exe_ctx->target->GetScratchClangASTContext()->getASTContext());
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     parser_type.GetASTContext(),
                                                     parser_type.GetOpaqueQualType()),
                           context);
    
    if (!m_parser_vars->m_persistent_vars->CreatePersistentVariable (exe_ctx->GetBestExecutionContextScope (),
                                                                     name, 
                                                                     user_type, 
                                                                     exe_ctx->process->GetByteOrder(),
                                                                     exe_ctx->process->GetAddressByteSize()))
        return false;
    
    ClangExpressionVariableSP var_sp (m_parser_vars->m_persistent_vars->GetVariable(name));
    
    if (!var_sp)
        return false;
    
    if (is_result)
        var_sp->m_flags |= ClangExpressionVariable::EVNeedsFreezeDry;
    else
        var_sp->m_flags |= ClangExpressionVariable::EVKeepInTarget; // explicitly-declared persistent variables should persist
    
    if (is_lvalue)
    {
        var_sp->m_flags |= ClangExpressionVariable::EVIsProgramReference;
    }
    else
    {
        var_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
        var_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
    }
    
    if (log)
        log->Printf("Created persistent variable with flags 0x%hx", var_sp->m_flags);
    
    var_sp->EnableParserVars();
    
    var_sp->m_parser_vars->m_named_decl = decl;
    var_sp->m_parser_vars->m_parser_type = parser_type;
    
    return true;
}

bool 
ClangExpressionDeclMap::AddValueToStruct 
(
    const NamedDecl *decl,
    const ConstString &name,
    llvm::Value *value,
    size_t size,
    off_t alignment
)
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    m_struct_vars->m_struct_laid_out = false;
    
    if (m_struct_members.GetVariable(decl))
        return true;
    
    ClangExpressionVariableSP var_sp (m_found_entities.GetVariable(decl));
    
    if (!var_sp)
        var_sp = m_parser_vars->m_persistent_vars->GetVariable(decl);
    
    if (!var_sp)
        return false;
    
    if (log)
        log->Printf("Adding value for decl %p [%s - %s] to the structure",
                    decl,
                    name.GetCString(),
                    var_sp->GetName().GetCString());
    
    // We know entity->m_parser_vars is valid because we used a parser variable
    // to find it
    var_sp->m_parser_vars->m_llvm_value = value;
    
    var_sp->EnableJITVars();
    var_sp->m_jit_vars->m_alignment = alignment;
    var_sp->m_jit_vars->m_size = size;
    
    m_struct_members.AddVariable(var_sp);
    
    return true;
}

bool
ClangExpressionDeclMap::DoStructLayout ()
{
    assert (m_struct_vars.get());
    
    if (m_struct_vars->m_struct_laid_out)
        return true;
    
    off_t cursor = 0;
    
    m_struct_vars->m_struct_alignment = 0;
    m_struct_vars->m_struct_size = 0;
    
    for (size_t member_index = 0, num_members = m_struct_members.GetSize();
         member_index < num_members;
         ++member_index)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_index));
        if (!member_sp)
            return false;

        if (!member_sp->m_jit_vars.get())
            return false;
        
        if (member_index == 0)
            m_struct_vars->m_struct_alignment = member_sp->m_jit_vars->m_alignment;
        
        if (cursor % member_sp->m_jit_vars->m_alignment)
            cursor += (member_sp->m_jit_vars->m_alignment - (cursor % member_sp->m_jit_vars->m_alignment));
        
        member_sp->m_jit_vars->m_offset = cursor;
        cursor += member_sp->m_jit_vars->m_size;
    }
    
    m_struct_vars->m_struct_size = cursor;
    
    m_struct_vars->m_struct_laid_out = true;
    return true;
}

bool ClangExpressionDeclMap::GetStructInfo 
(
    uint32_t &num_elements,
    size_t &size,
    off_t &alignment
)
{
    assert (m_struct_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
        return false;
    
    num_elements = m_struct_members.GetSize();
    size = m_struct_vars->m_struct_size;
    alignment = m_struct_vars->m_struct_alignment;
    
    return true;
}

bool 
ClangExpressionDeclMap::GetStructElement 
(
    const NamedDecl *&decl,
    llvm::Value *&value,
    off_t &offset,
    ConstString &name,
    uint32_t index
)
{
    assert (m_struct_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
        return false;
    
    if (index >= m_struct_members.GetSize())
        return false;
    
    ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(index));
    
    if (!member_sp ||
        !member_sp->m_parser_vars.get() ||
        !member_sp->m_jit_vars.get())
        return false;
    
    decl = member_sp->m_parser_vars->m_named_decl;
    value = member_sp->m_parser_vars->m_llvm_value;
    offset = member_sp->m_jit_vars->m_offset;
    name = member_sp->GetName();
        
    return true;
}

bool
ClangExpressionDeclMap::GetFunctionInfo 
(
    const NamedDecl *decl, 
    llvm::Value**& value, 
    uint64_t &ptr
)
{
    ClangExpressionVariableSP entity_sp(m_found_entities.GetVariable(decl));

    if (!entity_sp)
        return false;
    
    // We know m_parser_vars is valid since we searched for the variable by
    // its NamedDecl
    
    value = &entity_sp->m_parser_vars->m_llvm_value;
    ptr = entity_sp->m_parser_vars->m_lldb_value->GetScalar().ULongLong();
    
    return true;
}

bool
ClangExpressionDeclMap::GetFunctionAddress 
(
    const ConstString &name,
    uint64_t &func_addr
)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    // Back out in all cases where we're not fully initialized
    if (m_parser_vars->m_exe_ctx->target == NULL)
        return false;
    if (!m_parser_vars->m_sym_ctx.target_sp)
        return false;

    SymbolContextList sc_list;
    const bool include_symbols = true;
    const bool append = false;
    m_parser_vars->m_sym_ctx.FindFunctionsByName(name, include_symbols, append, sc_list);
    
    if (!sc_list.GetSize())
    {
        // We occasionally get debug information in which a const function is reported 
        // as non-const, so the mangled name is wrong.  This is a hack to compensate.
        
        Mangled mangled(name.GetCString(), true);
        
        ConstString demangled_name = mangled.GetDemangledName();
        
        if (strlen(demangled_name.GetCString()))
        {
            std::string const_name_scratch(demangled_name.GetCString());
            
            const_name_scratch.append(" const");
            
            ConstString const_name(const_name_scratch.c_str());
                        
            m_parser_vars->m_sym_ctx.FindFunctionsByName(const_name, include_symbols, append, sc_list);
            
            if (log)
                log->Printf("Found %d results with const name %s", sc_list.GetSize(), const_name.GetCString());
        }
    }
    
    if (!sc_list.GetSize())
        return false;
    
    SymbolContext sym_ctx;
    sc_list.GetContextAtIndex(0, sym_ctx);
    
    const Address *func_so_addr = NULL;
    
    if (sym_ctx.function)
        func_so_addr = &sym_ctx.function->GetAddressRange().GetBaseAddress();
    else if (sym_ctx.symbol)
        func_so_addr = &sym_ctx.symbol->GetAddressRangeRef().GetBaseAddress();
    else
        return false;
    
    if (!func_so_addr || !func_so_addr->IsValid())
        return false;
    
    func_addr = func_so_addr->GetCallableLoadAddress (m_parser_vars->m_exe_ctx->target);

    return true;
}

addr_t
ClangExpressionDeclMap::GetSymbolAddress (Target &target, const ConstString &name)
{
    SymbolContextList sc_list;
    
    target.GetImages().FindSymbolsWithNameAndType(name, eSymbolTypeAny, sc_list);
    
    const uint32_t num_matches = sc_list.GetSize();
    addr_t symbol_load_addr = LLDB_INVALID_ADDRESS;

    for (uint32_t i=0; i<num_matches && symbol_load_addr == LLDB_INVALID_ADDRESS; i++)
    {
        SymbolContext sym_ctx;
        sc_list.GetContextAtIndex(i, sym_ctx);
    
        const Address *sym_address = &sym_ctx.symbol->GetAddressRangeRef().GetBaseAddress();
        
        if (!sym_address || !sym_address->IsValid())
            return LLDB_INVALID_ADDRESS;
        
        if (sym_address)
        {
            switch (sym_ctx.symbol->GetType())
            {
                case eSymbolTypeCode:
                case eSymbolTypeTrampoline:
                    symbol_load_addr = sym_address->GetCallableLoadAddress (&target);
                    break;
                    
                case eSymbolTypeData:
                case eSymbolTypeRuntime:
                case eSymbolTypeVariable:
                case eSymbolTypeLocal:
                case eSymbolTypeParam:
                case eSymbolTypeInvalid:
                case eSymbolTypeAbsolute:
                case eSymbolTypeExtern:
                case eSymbolTypeException:
                case eSymbolTypeSourceFile:
                case eSymbolTypeHeaderFile:
                case eSymbolTypeObjectFile:
                case eSymbolTypeCommonBlock:
                case eSymbolTypeBlock:
                case eSymbolTypeVariableType:
                case eSymbolTypeLineEntry:
                case eSymbolTypeLineHeader:
                case eSymbolTypeScopeBegin:
                case eSymbolTypeScopeEnd:
                case eSymbolTypeAdditional:
                case eSymbolTypeCompiler:
                case eSymbolTypeInstrumentation:
                case eSymbolTypeUndefined:
                    symbol_load_addr = sym_address->GetLoadAddress (&target);
                    break;
            }
        }
    }
    
    return symbol_load_addr;
}

addr_t
ClangExpressionDeclMap::GetSymbolAddress (const ConstString &name)
{
    assert (m_parser_vars.get());
    
    if (!m_parser_vars->m_exe_ctx ||
        !m_parser_vars->m_exe_ctx->target)
        return false;
    
    return GetSymbolAddress(*m_parser_vars->m_exe_ctx->target, name);
}

// Interface for CommandObjectExpression

bool 
ClangExpressionDeclMap::Materialize 
(
    ExecutionContext &exe_ctx, 
    lldb::addr_t &struct_address,
    Error &err
)
{
    EnableMaterialVars();
    
    m_material_vars->m_process = exe_ctx.process;
    
    bool result = DoMaterialize(false /* dematerialize */, 
                                exe_ctx, 
                                LLDB_INVALID_ADDRESS /* top of stack frame */, 
                                LLDB_INVALID_ADDRESS /* bottom of stack frame */, 
                                NULL, /* result SP */
                                err);
    
    if (result)
        struct_address = m_material_vars->m_materialized_location;
    
    return result;
}

bool 
ClangExpressionDeclMap::GetObjectPointer
(
    lldb::addr_t &object_ptr,
    ConstString &object_name,
    ExecutionContext &exe_ctx,
    Error &err,
    bool suppress_type_check
)
{
    assert (m_struct_vars.get());
    
    if (!exe_ctx.frame || !exe_ctx.target || !exe_ctx.process)
    {
        err.SetErrorString("Couldn't load 'this' because the context is incomplete");
        return false;
    }
    
    if (!m_struct_vars->m_object_pointer_type.GetOpaqueQualType())
    {
        err.SetErrorString("Couldn't load 'this' because its type is unknown");
        return false;
    }
    
    VariableSP object_ptr_var = FindVariableInScope (*exe_ctx.frame,
                                                     object_name, 
                                                     (suppress_type_check ? NULL : &m_struct_vars->m_object_pointer_type));
    
    if (!object_ptr_var)
    {
        err.SetErrorStringWithFormat("Couldn't find '%s' with appropriate type in scope", object_name.GetCString());
        return false;
    }
    
    std::auto_ptr<lldb_private::Value> location_value(GetVariableValue(exe_ctx,
                                                                       object_ptr_var,
                                                                       NULL));
    
    if (!location_value.get())
    {
        err.SetErrorStringWithFormat("Couldn't get the location for '%s'", object_name.GetCString());
        return false;
    }
    
    switch (location_value->GetValueType())
    {
    default:
        err.SetErrorStringWithFormat("'%s' is not in memory; LLDB must be extended to handle registers", object_name.GetCString());
        return false;
    case Value::eValueTypeLoadAddress:
        {
            lldb::addr_t value_addr = location_value->GetScalar().ULongLong();
            uint32_t address_byte_size = exe_ctx.target->GetArchitecture().GetAddressByteSize();
            
            if (ClangASTType::GetClangTypeBitWidth(m_struct_vars->m_object_pointer_type.GetASTContext(), 
                                                   m_struct_vars->m_object_pointer_type.GetOpaqueQualType()) != address_byte_size * 8)
            {
                err.SetErrorStringWithFormat("'%s' is not of an expected pointer size", object_name.GetCString());
                return false;
            }
            
            Error read_error;
            object_ptr = exe_ctx.process->ReadPointerFromMemory (value_addr, read_error);
            if (read_error.Fail() || object_ptr == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Coldn't read '%s' from the target: %s", object_name.GetCString(), read_error.AsCString());
                return false;
            }            
            return true;
        }
    case Value::eValueTypeScalar:
        {
            if (location_value->GetContextType() != Value::eContextTypeRegisterInfo)
            {
                StreamString ss;
                location_value->Dump(&ss);
                
                err.SetErrorStringWithFormat("%s is a scalar of unhandled type: %s", object_name.GetCString(), ss.GetString().c_str());
                return false;
            }
                        
            RegisterInfo *reg_info = location_value->GetRegisterInfo();
            
            if (!reg_info)
            {
                err.SetErrorStringWithFormat("Couldn't get the register information for %s", object_name.GetCString());
                return false;
            }
            
            RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
            
            if (!reg_ctx)
            {
                err.SetErrorStringWithFormat("Couldn't read register context to read %s from %s", object_name.GetCString(), reg_info->name);
                return false;
            }
            
            uint32_t register_number = reg_info->kinds[lldb::eRegisterKindLLDB];
            
            object_ptr = reg_ctx->ReadRegisterAsUnsigned(register_number, 0x0);
            
            return true;
        }
    }
}

bool 
ClangExpressionDeclMap::Dematerialize 
(
    ExecutionContext &exe_ctx,
    ClangExpressionVariableSP &result_sp,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    Error &err
)
{
    return DoMaterialize(true, exe_ctx, stack_frame_top, stack_frame_bottom, &result_sp, err);
    
    DidDematerialize();
}

void
ClangExpressionDeclMap::DidDematerialize()
{
    if (m_material_vars.get())
    {
        if (m_material_vars->m_materialized_location)
        {        
            //#define SINGLE_STEP_EXPRESSIONS
            
#ifndef SINGLE_STEP_EXPRESSIONS
            m_material_vars->m_process->DeallocateMemory(m_material_vars->m_materialized_location);
#endif
            m_material_vars->m_materialized_location = 0;
        }
        
        DisableMaterialVars();
    }
}

bool
ClangExpressionDeclMap::DumpMaterializedStruct
(
    ExecutionContext &exe_ctx, 
    Stream &s,
    Error &err
)
{
    assert (m_struct_vars.get());
    assert (m_material_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
    {
        err.SetErrorString("Structure hasn't been laid out yet");
        return false;
    }
    
    if (!exe_ctx.process)
    {
        err.SetErrorString("Couldn't find the process");
        return false;
    }
    
    if (!exe_ctx.target)
    {
        err.SetErrorString("Couldn't find the target");
        return false;
    }
    
    if (!m_material_vars->m_materialized_location)
    {
        err.SetErrorString("No materialized location");
        return false;
    }
    
    lldb::DataBufferSP data_sp(new DataBufferHeap(m_struct_vars->m_struct_size, 0));    
    
    Error error;
    if (exe_ctx.process->ReadMemory (m_material_vars->m_materialized_location, 
                                     data_sp->GetBytes(), 
                                     data_sp->GetByteSize(), error) != data_sp->GetByteSize())
    {
        err.SetErrorStringWithFormat ("Couldn't read struct from the target: %s", error.AsCString());
        return false;
    }
    
    DataExtractor extractor(data_sp, exe_ctx.process->GetByteOrder(), exe_ctx.target->GetArchitecture().GetAddressByteSize());
    
    for (size_t member_idx = 0, num_members = m_struct_members.GetSize();
         member_idx < num_members;
         ++member_idx)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_idx));
        
        if (!member_sp)
            return false;

        s.Printf("[%s]\n", member_sp->GetName().GetCString());
        
        if (!member_sp->m_jit_vars.get())
            return false;
        
        extractor.Dump (&s,                                                                          // stream
                        member_sp->m_jit_vars->m_offset,                                             // offset
                        lldb::eFormatBytesWithASCII,                                                 // format
                        1,                                                                           // byte size of individual entries
                        member_sp->m_jit_vars->m_size,                                               // number of entries
                        16,                                                                          // entries per line
                        m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset,  // address to print
                        0,                                                                           // bit size (bitfields only; 0 means ignore)
                        0);                                                                          // bit alignment (bitfields only; 0 means ignore)
        
        s.PutChar('\n');
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterialize 
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    lldb::ClangExpressionVariableSP *result_sp_ptr,
    Error &err
)
{
    if (result_sp_ptr)
        result_sp_ptr->reset();

    assert (m_struct_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    if (!m_struct_vars->m_struct_laid_out)
    {
        err.SetErrorString("Structure hasn't been laid out yet");
        return LLDB_INVALID_ADDRESS;
    }
    
    if (!exe_ctx.frame)
    {
        err.SetErrorString("Received null execution frame");
        return LLDB_INVALID_ADDRESS;
    }
    
    ClangPersistentVariables &persistent_vars = exe_ctx.target->GetPersistentVariables();
        
    if (!m_struct_vars->m_struct_size)
    {
        if (log)
            log->PutCString("Not bothering to allocate a struct because no arguments are needed");
        
        m_material_vars->m_allocated_area = NULL;
        
        return true;
    }
    
    const SymbolContext &sym_ctx(exe_ctx.frame->GetSymbolContext(lldb::eSymbolContextEverything));
    
    if (!dematerialize)
    {
        if (m_material_vars->m_materialized_location)
        {
            exe_ctx.process->DeallocateMemory(m_material_vars->m_materialized_location);
            m_material_vars->m_materialized_location = 0;
        }
        
        if (log)
            log->PutCString("Allocating memory for materialized argument struct");
        
        lldb::addr_t mem = exe_ctx.process->AllocateMemory(m_struct_vars->m_struct_alignment + m_struct_vars->m_struct_size, 
                                                           lldb::ePermissionsReadable | lldb::ePermissionsWritable,
                                                           err);
        
        if (mem == LLDB_INVALID_ADDRESS)
            return false;
        
        m_material_vars->m_allocated_area = mem;
    }
    
    m_material_vars->m_materialized_location = m_material_vars->m_allocated_area;
    
    if (m_material_vars->m_materialized_location % m_struct_vars->m_struct_alignment)
        m_material_vars->m_materialized_location += (m_struct_vars->m_struct_alignment - (m_material_vars->m_materialized_location % m_struct_vars->m_struct_alignment));
    
    for (uint64_t member_index = 0, num_members = m_struct_members.GetSize();
         member_index < num_members;
         ++member_index)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_index));
        
        if (m_found_entities.ContainsVariable (member_sp))
        {
            RegisterInfo *reg_info = member_sp->GetRegisterInfo ();
            if (reg_info)
            {
                // This is a register variable
                
                RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
                
                if (!reg_ctx)
                    return false;
                
                if (!DoMaterializeOneRegister (dematerialize, 
                                               exe_ctx, 
                                               *reg_ctx, 
                                               *reg_info, 
                                               m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset, 
                                               err))
                    return false;
            }
            else
            {
                if (!member_sp->m_jit_vars.get())
                    return false;
                
                if (!DoMaterializeOneVariable (dematerialize, 
                                               exe_ctx, 
                                               sym_ctx,
                                               member_sp,
                                               m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset, 
                                               err))
                    return false;
            }
        }
        else
        {
            // No need to look for presistent variables if the name doesn't start 
            // with with a '$' character...
            if (member_sp->GetName().AsCString ("!")[0] == '$' && persistent_vars.ContainsVariable(member_sp))
            {
                
                if (member_sp->GetName() == m_struct_vars->m_result_name)
                {
                    if (log)
                        log->PutCString("Found result member in the struct");

                    if (result_sp_ptr)
                        *result_sp_ptr = member_sp;
                    
                }

                if (!DoMaterializeOnePersistentVariable (dematerialize, 
                                                         exe_ctx,
                                                         member_sp, 
                                                         m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset,
                                                         stack_frame_top,
                                                         stack_frame_bottom,
                                                         err))
                    return false;
            }
            else
            {
                err.SetErrorStringWithFormat("Unexpected variable %s", member_sp->GetName().GetCString());
                return false;
            }
        }
    }
    
    return true;
}

bool
ClangExpressionDeclMap::DoMaterializeOnePersistentVariable
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    ClangExpressionVariableSP &var_sp,
    lldb::addr_t addr,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    Error &err
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    if (!var_sp)
    {
        err.SetErrorString("Invalid persistent variable");
        return LLDB_INVALID_ADDRESS;
    }
    
    const size_t pvar_byte_size = var_sp->GetByteSize();
    
    uint8_t *pvar_data = var_sp->GetValueBytes();
    if (pvar_data == NULL)
        return false;
    
    Error error;
    
    lldb::addr_t mem; // The address of a spare memory area used to hold the persistent variable.
    
    if (dematerialize)
    {
        if (log)
            log->Printf("Dematerializing persistent variable with flags 0x%hx", var_sp->m_flags);
        
        if ((var_sp->m_flags & ClangExpressionVariable::EVIsLLDBAllocated) ||
            (var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference))
        {
            // Get the location of the target out of the struct.
            
            Error read_error;
            mem = exe_ctx.process->ReadPointerFromMemory (addr, read_error);
            
            if (mem == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Couldn't read address of %s from struct: %s", var_sp->GetName().GetCString(), error.AsCString());
                return false;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference &&
                !var_sp->m_live_sp)
            {
                // If the reference comes from the program, then the ClangExpressionVariable's
                // live variable data hasn't been set up yet.  Do this now.
                
                var_sp->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope (),
                                                                    var_sp->GetTypeFromUser().GetASTContext(),
                                                                    var_sp->GetTypeFromUser().GetOpaqueQualType(),
                                                                    var_sp->GetName(),
                                                                    mem,
                                                                    eAddressTypeLoad,
                                                                    pvar_byte_size);
            }
            
            if (!var_sp->m_live_sp)
            {
                err.SetErrorStringWithFormat("Couldn't find the memory area used to store %s", var_sp->GetName().GetCString());
                return false;
            }
            
            if (var_sp->m_live_sp->GetValue().GetValueAddressType() != eAddressTypeLoad)
            {
                err.SetErrorStringWithFormat("The address of the memory area for %s is in an incorrect format", var_sp->GetName().GetCString());
                return false;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVNeedsFreezeDry ||
                var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget)
            {
                mem = var_sp->m_live_sp->GetValue().GetScalar().ULongLong();
                
                if (log)
                    log->Printf("Dematerializing %s from 0x%llx", var_sp->GetName().GetCString(), (uint64_t)mem);
                
                // Read the contents of the spare memory area
                                
                var_sp->ValueUpdated ();
                if (exe_ctx.process->ReadMemory (mem, pvar_data, pvar_byte_size, error) != pvar_byte_size)
                {
                    err.SetErrorStringWithFormat ("Couldn't read a composite type from the target: %s", error.AsCString());
                    return false;
                }
                
                if (stack_frame_top != LLDB_INVALID_ADDRESS &&
                    stack_frame_bottom != LLDB_INVALID_ADDRESS &&
                    mem >= stack_frame_bottom &&
                    mem <= stack_frame_top)
                {
                    // If the variable is resident in the stack frame created by the expression,
                    // then it cannot be relied upon to stay around.  We treat it as needing
                    // reallocation.
                    
                    var_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
                    var_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
                    var_sp->m_flags &= ~ClangExpressionVariable::EVIsProgramReference;
                }
                
                var_sp->m_flags &= ~ClangExpressionVariable::EVNeedsFreezeDry;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVNeedsAllocation &&
                !(var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget))
            {
                if (m_keep_result_in_memory)
                {
                    var_sp->m_flags |= ClangExpressionVariable::EVKeepInTarget;
                }
                else
                {
                    Error deallocate_error = exe_ctx.process->DeallocateMemory(mem);
                    
                    if (!err.Success())
                    {
                        err.SetErrorStringWithFormat ("Couldn't deallocate memory for %s: %s", var_sp->GetName().GetCString(), deallocate_error.AsCString());
                        return false;
                    }
                }
            }
        }
        else
        {
            err.SetErrorStringWithFormat("Persistent variables without separate allocations are not currently supported.");
            return false;
        }
    }
    else 
    {
        if (log)
            log->Printf("Materializing persistent variable with flags 0x%hx", var_sp->m_flags);
        
        if (var_sp->m_flags & ClangExpressionVariable::EVNeedsAllocation)
        {
            // Allocate a spare memory area to store the persistent variable's contents.
            
            Error allocate_error;
            
            mem = exe_ctx.process->AllocateMemory(pvar_byte_size, 
                                                  lldb::ePermissionsReadable | lldb::ePermissionsWritable, 
                                                  allocate_error);
            
            if (mem == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Couldn't allocate a memory area to store %s: %s", var_sp->GetName().GetCString(), allocate_error.AsCString());
                return false;
            }
            
            if (log)
                log->Printf("Allocated %s (0x%llx) sucessfully", var_sp->GetName().GetCString(), mem);
            
            // Put the location of the spare memory into the live data of the ValueObject.
            
            var_sp->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope(),
                                                                var_sp->GetTypeFromUser().GetASTContext(),
                                                                var_sp->GetTypeFromUser().GetOpaqueQualType(),
                                                                var_sp->GetName(),
                                                                mem,
                                                                eAddressTypeLoad,
                                                                pvar_byte_size);
            
            // Clear the flag if the variable will never be deallocated.
            
            if (var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget)
                var_sp->m_flags &= ~ClangExpressionVariable::EVNeedsAllocation;
            
            // Write the contents of the variable to the area.
            
            if (exe_ctx.process->WriteMemory (mem, pvar_data, pvar_byte_size, error) != pvar_byte_size)
            {
                err.SetErrorStringWithFormat ("Couldn't write a composite type to the target: %s", error.AsCString());
                return false;
            }
        }
        
        if ((var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference && var_sp->m_live_sp) ||
            var_sp->m_flags & ClangExpressionVariable::EVIsLLDBAllocated)
        {
            // Now write the location of the area into the struct.
            Error write_error;
            if (!exe_ctx.process->WriteScalarToMemory (addr, 
                                                       var_sp->m_live_sp->GetValue().GetScalar(), 
                                                       exe_ctx.process->GetAddressByteSize(), 
                                                       write_error))
            {
                err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", var_sp->GetName().GetCString(), write_error.AsCString());
                return false;
            }
            
            if (log)
                log->Printf("Materialized %s into 0x%llx", var_sp->GetName().GetCString(), var_sp->m_live_sp->GetValue().GetScalar().ULongLong());
        }
        else if (!(var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference))
        {
            err.SetErrorStringWithFormat("Persistent variables without separate allocations are not currently supported.");
            return false;
        }
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterializeOneVariable
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    const SymbolContext &sym_ctx,
    ClangExpressionVariableSP &expr_var,
    lldb::addr_t addr, 
    Error &err
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    if (!exe_ctx.frame || !exe_ctx.process)
        return false;
    
    // Vital information about the value
    
    const ConstString &name(expr_var->GetName());
    TypeFromUser type(expr_var->GetTypeFromUser());
    
    VariableSP var = FindVariableInScope (*exe_ctx.frame, name, &type);
    Symbol *sym = FindGlobalDataSymbol(*exe_ctx.target, name);
    
    std::auto_ptr<lldb_private::Value> location_value;
    
    if (var)
    {
        location_value.reset(GetVariableValue(exe_ctx,
                                              var,
                                              NULL));
    }
    else if (sym)
    {        
        location_value.reset(new Value);
        
        addr_t location_load_addr = GetSymbolAddress(*exe_ctx.target, name);
        
        if (location_load_addr == LLDB_INVALID_ADDRESS)
        {
            if (log)
                err.SetErrorStringWithFormat ("Couldn't find value for global symbol %s", 
                                              name.GetCString());
        }
        
        location_value->SetValueType(Value::eValueTypeLoadAddress);
        location_value->GetScalar() = location_load_addr;
    }
    else
    {
        err.SetErrorStringWithFormat ("Couldn't find %s with appropriate type", 
                                      name.GetCString());
        return false;
    }
    
    if (log)
    {
        StreamString my_stream_string;
        
        ClangASTType::DumpTypeDescription (type.GetASTContext(),
                                           type.GetOpaqueQualType(),
                                           &my_stream_string);
        
        log->Printf ("%s %s with type %s", 
                     dematerialize ? "Dematerializing" : "Materializing", 
                     name.GetCString(), 
                     my_stream_string.GetString().c_str());
    }
    
    if (!location_value.get())
    {
        err.SetErrorStringWithFormat("Couldn't get value for %s", name.GetCString());
        return false;
    }

    // The size of the type contained in addr
    
    size_t value_bit_size = ClangASTType::GetClangTypeBitWidth(type.GetASTContext(), type.GetOpaqueQualType());
    size_t value_byte_size = value_bit_size % 8 ? ((value_bit_size + 8) / 8) : (value_bit_size / 8);
    
    Value::ValueType value_type = location_value->GetValueType();
    
    switch (value_type)
    {
    default:
        {
            StreamString ss;
            
            location_value->Dump(&ss);
            
            err.SetErrorStringWithFormat ("%s has a value of unhandled type: %s", 
                                          name.GetCString(), 
                                          ss.GetString().c_str());
            return false;
        }
        break;
    case Value::eValueTypeLoadAddress:
        {
            if (!dematerialize)
            {
                Error write_error;

                if (!exe_ctx.process->WriteScalarToMemory (addr, 
                                                           location_value->GetScalar(), 
                                                           exe_ctx.process->GetAddressByteSize(), 
                                                           write_error))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", 
                                                  name.GetCString(), 
                                                  write_error.AsCString());
                    return false;
                }
            }
        }
        break;
    case Value::eValueTypeScalar:
        {
            if (location_value->GetContextType() != Value::eContextTypeRegisterInfo)
            {
                StreamString ss;
                location_value->Dump(&ss);
                
                err.SetErrorStringWithFormat ("%s is a scalar of unhandled type: %s", 
                                              name.GetCString(), 
                                              ss.GetString().c_str());
                return false;
            }
            
            RegisterInfo *reg_info = location_value->GetRegisterInfo();
            
            if (!reg_info)
            {
                err.SetErrorStringWithFormat ("Couldn't get the register information for %s", 
                                              name.GetCString());
                return false;
            }
            
            RegisterValue reg_value;

            RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
            
            if (!reg_ctx)
            {
                err.SetErrorStringWithFormat ("Couldn't read register context to read %s from %s", 
                                              name.GetCString(), 
                                              reg_info->name);
                return false;
            }
            
            uint32_t register_byte_size = reg_info->byte_size;
            
            if (dematerialize)
            {
                // Get the location of the spare memory area out of the variable's live data.
                
                if (!expr_var->m_live_sp)
                {
                    err.SetErrorStringWithFormat("Couldn't find the memory area used to store %s", name.GetCString());
                    return false;
                }
                
                if (expr_var->m_live_sp->GetValue().GetValueAddressType() != eAddressTypeLoad)
                {
                    err.SetErrorStringWithFormat("The address of the memory area for %s is in an incorrect format", name.GetCString());
                    return false;
                }
                
                Scalar &reg_addr = expr_var->m_live_sp->GetValue().GetScalar();
                
                err = reg_ctx->ReadRegisterValueFromMemory (reg_info, 
                                                            reg_addr.ULongLong(), 
                                                            value_byte_size, 
                                                            reg_value);
                if (err.Fail())
                    return false;

                if (!reg_ctx->WriteRegister (reg_info, reg_value))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to register %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }
                
                // Deallocate the spare area and clear the variable's live data.
                
                Error deallocate_error = exe_ctx.process->DeallocateMemory(reg_addr.ULongLong());
                
                if (!deallocate_error.Success())
                {
                    err.SetErrorStringWithFormat ("Couldn't deallocate spare memory area for %s: %s", 
                                                  name.GetCString(), 
                                                  deallocate_error.AsCString());
                    return false;
                }
                
                expr_var->m_live_sp.reset();
            }
            else
            {
                // Allocate a spare memory area to place the register's contents into.  This memory area will be pointed to by the slot in the
                // struct.
                
                Error allocate_error;
                
                Scalar reg_addr (exe_ctx.process->AllocateMemory (value_byte_size, 
                                                                  lldb::ePermissionsReadable | lldb::ePermissionsWritable, 
                                                                  allocate_error));
                
                if (reg_addr.ULongLong() == LLDB_INVALID_ADDRESS)
                {
                    err.SetErrorStringWithFormat ("Couldn't allocate a memory area to store %s: %s", 
                                                  name.GetCString(), 
                                                  allocate_error.AsCString());
                    return false;
                }
                
                // Put the location of the spare memory into the live data of the ValueObject.
                
                expr_var->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope(),
                                                                      type.GetASTContext(),
                                                                      type.GetOpaqueQualType(),
                                                                      name,
                                                                      reg_addr.ULongLong(),
                                                                      eAddressTypeLoad,
                                                                      value_byte_size);
                
                // Now write the location of the area into the struct.
                
                Error write_error;
                
                if (!exe_ctx.process->WriteScalarToMemory (addr, 
                                                           reg_addr, 
                                                           exe_ctx.process->GetAddressByteSize(), 
                                                           write_error))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", 
                                                  name.GetCString(), 
                                                  write_error.AsCString());
                    return false;
                }
                
                if (value_byte_size > register_byte_size)
                {
                    err.SetErrorStringWithFormat ("%s is too big to store in %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }

                RegisterValue reg_value;

                if (!reg_ctx->ReadRegister (reg_info, reg_value))
                {
                    err.SetErrorStringWithFormat ("Couldn't read %s from %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }
                
                err = reg_ctx->WriteRegisterValueToMemory (reg_info, 
                                                           reg_addr.ULongLong(), 
                                                           value_byte_size, 
                                                           reg_value);
                if (err.Fail())
                    return false;
            }
        }
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterializeOneRegister
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    RegisterContext &reg_ctx,
    const RegisterInfo &reg_info,
    lldb::addr_t addr, 
    Error &err
)
{
    uint32_t register_byte_size = reg_info.byte_size;
    RegisterValue reg_value;
    if (dematerialize)
    {
        Error read_error (reg_ctx.ReadRegisterValueFromMemory(&reg_info, addr, register_byte_size, reg_value));
        if (read_error.Fail())
        {
            err.SetErrorStringWithFormat ("Couldn't read %s from the target: %s", reg_info.name, read_error.AsCString());
            return false;
        }
        
        if (!reg_ctx.WriteRegister (&reg_info, reg_value))
        {
            err.SetErrorStringWithFormat("Couldn't write register %s (dematerialize)", reg_info.name);
            return false;
        }
    }
    else
    {
        
        if (!reg_ctx.ReadRegister(&reg_info, reg_value))
        {
            err.SetErrorStringWithFormat("Couldn't read %s (materialize)", reg_info.name);
            return false;
        }
        
        Error write_error (reg_ctx.WriteRegisterValueToMemory(&reg_info, addr, register_byte_size, reg_value));
        if (write_error.Fail())
        {
            err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", write_error.AsCString());
            return false;
        }
    }
    
    return true;
}

lldb::VariableSP
ClangExpressionDeclMap::FindVariableInScope
(
    StackFrame &frame,
    const ConstString &name,
    TypeFromUser *type
)
{    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ValueObjectSP valobj;
    VariableSP var_sp;
    Error err;
    
    valobj = frame.GetValueForVariableExpressionPath(name.GetCString(), 
                                                     eNoDynamicValues, 
                                                     StackFrame::eExpressionPathOptionCheckPtrVsMember,
                                                     var_sp,
                                                     err);
        
    if (!err.Success() ||
        !var_sp ||
        !var_sp->IsInScope(&frame) ||
        !var_sp->LocationIsValidForFrame (&frame))
        return lldb::VariableSP();

    if (var_sp && type)
    {
        if (type->GetASTContext() == var_sp->GetType()->GetClangAST())
        {
            if (!ClangASTContext::AreTypesSame(type->GetASTContext(), type->GetOpaqueQualType(), var_sp->GetType()->GetClangFullType()))
                return lldb::VariableSP();
        }
        else
        {
            if (log)
                log->PutCString("Skipping a candidate variable because of different AST contexts");
            return lldb::VariableSP();
        }
    }

    return var_sp;
}

Symbol *
ClangExpressionDeclMap::FindGlobalDataSymbol
(
    Target &target,
    const ConstString &name
)
{
    SymbolContextList sc_list;
    
    target.GetImages().FindSymbolsWithNameAndType(name, 
                                                   eSymbolTypeData, 
                                                   sc_list);
    
    if (sc_list.GetSize())
    {
        SymbolContext sym_ctx;
        sc_list.GetContextAtIndex(0, sym_ctx);
        
        return sym_ctx.symbol;
    }
    
    return NULL;
}

// Interface for ClangASTSource
void 
ClangExpressionDeclMap::GetDecls (NameSearchContext &context, const ConstString &name)
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
        
    if (log)
        log->Printf("Hunting for a definition for '%s'", name.GetCString());
    
    // Back out in all cases where we're not fully initialized
    if (m_parser_vars->m_exe_ctx->frame == NULL)
        return;
    
    if (m_parser_vars->m_ignore_lookups)
    {
        if (log)
            log->Printf("Ignoring a query during an import");
        return;
    }
    
    do
    {
        if (isa<TranslationUnitDecl>(context.m_decl_context))
            break;
        
        if (!isa<NamespaceDecl>(context.m_decl_context))
            return;
        
        const Decl *context_decl = dyn_cast<Decl>(context.m_decl_context);
        
        if (log)
            log->Printf("Searching for '%s' in a '%s'", name.GetCString(), context_decl->getDeclKindName());
        
        Decl *original_decl = NULL;
        ASTContext *original_ctx = NULL;
        
        if (!m_parser_vars->GetASTImporter(context.GetASTContext())->ResolveDeclOrigin(context_decl, &original_decl, &original_ctx))
            break;
        
        if (TagDecl *original_tag_decl = dyn_cast<TagDecl>(original_decl))
        {
            ExternalASTSource *external_source = original_ctx->getExternalSource();
            
            if (!external_source)
                break;
                        
            if (!original_tag_decl)
                break;
            
            external_source->CompleteType (original_tag_decl);
        }
        
        DeclContext *original_decl_context = dyn_cast<DeclContext>(original_decl);
        
        if (!original_decl_context)
            break;
                
        for (TagDecl::decl_iterator iter = original_decl_context->decls_begin();
             iter != original_decl_context->decls_end();
             ++iter)
        {
            NamedDecl *named_decl = dyn_cast<NamedDecl>(*iter);
            
            if (named_decl && named_decl->getName().equals(name.GetCString()))
            {
                Decl *copied_decl = m_parser_vars->GetASTImporter(context.GetASTContext())->CopyDecl(original_ctx, named_decl);
                NamedDecl *copied_named_decl = dyn_cast<NamedDecl>(copied_decl);
                
                if (!copied_named_decl)
                    continue;
                
                context.AddNamedDecl (copied_named_decl);
            }
        }
        
        return;
    }
    while (0);
        
    SymbolContextList sc_list;
    
    const char *name_unique_cstr = name.GetCString();
    
    if (name_unique_cstr == NULL)
        return;

    // Only look for functions by name out in our symbols if the function 
    // doesn't start with our phony prefix of '$'
    if (name_unique_cstr[0] != '$')
    {
        ValueObjectSP valobj;
        VariableSP var;
        Error err;
        
        valobj = m_parser_vars->m_exe_ctx->frame->GetValueForVariableExpressionPath(name_unique_cstr, 
                                                                                    eNoDynamicValues, 
                                                                                    StackFrame::eExpressionPathOptionCheckPtrVsMember,
                                                                                    var,
                                                                                    err);
        
        // If we found a variable in scope, no need to pull up function names
        if (err.Success() && var != NULL)
        {
            AddOneVariable(context, var);
        }
        else
        {
            const bool include_symbols = true;
            const bool append = false;
            m_parser_vars->m_sym_ctx.FindFunctionsByName (name, 
                                                          include_symbols, 
                                                          append, 
                                                          sc_list);
        
            if (sc_list.GetSize())
            {
                bool found_specific = false;
                Symbol *generic_symbol = NULL;
                Symbol *non_extern_symbol = NULL;
                
                for (uint32_t index = 0, num_indices = sc_list.GetSize();
                     index < num_indices;
                     ++index)
                {
                    SymbolContext sym_ctx;
                    sc_list.GetContextAtIndex(index, sym_ctx);
                    
                    if (sym_ctx.function)
                    {
                        // TODO only do this if it's a C function; C++ functions may be
                        // overloaded
                        if (!found_specific)
                            AddOneFunction(context, sym_ctx.function, NULL);
                        found_specific = true;
                    }
                    else if (sym_ctx.symbol)
                    {
                        if (sym_ctx.symbol->IsExternal())
                            generic_symbol = sym_ctx.symbol;
                        else
                            non_extern_symbol = sym_ctx.symbol;
                    }
                }
                
                if (!found_specific)
                {
                    if (generic_symbol)
                        AddOneFunction (context, NULL, generic_symbol);
                    else if (non_extern_symbol)
                        AddOneFunction (context, NULL, non_extern_symbol);
                }
            }
            else
            {
                // We couldn't find a variable or function for this.  Now we'll hunt for a generic 
                // data symbol, and -- if it is found -- treat it as a variable.
                
                Symbol *data_symbol = FindGlobalDataSymbol(*m_parser_vars->m_exe_ctx->target, name);
                
                if (data_symbol)
                    AddOneGenericVariable(context, *data_symbol);
            }
        }
        
        ClangNamespaceDecl namespace_decl (m_parser_vars->m_sym_ctx.FindNamespace(name));
        
        if (namespace_decl)
        {
            if (log)
            {                
                std::string s;
                llvm::raw_string_ostream os(s);
                namespace_decl.GetNamespaceDecl()->print(os);
                os.flush();
                
                log->Printf("Added namespace decl:");
                log->Printf("%s", s.c_str());
            }
            
            NamespaceDecl *clang_namespace_decl = AddNamespace(context, namespace_decl);
            if (clang_namespace_decl)
                clang_namespace_decl->setHasExternalLexicalStorage();
        }
    }
    else
    {
        static ConstString g_lldb_class_name ("$__lldb_class");
        if (name == g_lldb_class_name)
        {
            // Clang is looking for the type of "this"
            
            VariableList *vars = m_parser_vars->m_exe_ctx->frame->GetVariableList(false);
            
            if (!vars)
                return;
            
            lldb::VariableSP this_var = vars->FindVariable(ConstString("this"));
            
            if (!this_var ||
                !this_var->IsInScope(m_parser_vars->m_exe_ctx->frame) || 
                !this_var->LocationIsValidForFrame (m_parser_vars->m_exe_ctx->frame))
                return;
            
            Type *this_type = this_var->GetType();
            
            if (!this_type)
                return;
            
            if (log)
            {
                log->PutCString ("Type for \"this\" is: ");
                StreamString strm;
                this_type->Dump(&strm, true);
                log->PutCString (strm.GetData());
            }

            TypeFromUser this_user_type(this_type->GetClangFullType(),
                                        this_type->GetClangAST());
            
            m_struct_vars->m_object_pointer_type = this_user_type;
            
            void *pointer_target_type = NULL;
            
            if (!ClangASTContext::IsPointerType(this_user_type.GetOpaqueQualType(),
                                                &pointer_target_type))
                return;
            
            clang::QualType pointer_target_qual_type = QualType::getFromOpaquePtr(pointer_target_type);
            
            if (pointer_target_qual_type.isConstQualified())
                pointer_target_qual_type.removeLocalConst();
            
            TypeFromUser class_user_type(pointer_target_qual_type.getAsOpaquePtr(),
                                         this_type->GetClangAST());

            if (log)
            {
                StreamString type_stream;
                class_user_type.DumpTypeCode(&type_stream);
                type_stream.Flush();
                log->Printf("Adding type for $__lldb_class: %s", type_stream.GetString().c_str());
            }
            
            AddOneType(context, class_user_type, true);
            
            return;
        }
        
        static ConstString g_lldb_objc_class_name ("$__lldb_objc_class");
        if (name == g_lldb_objc_class_name)
        {
            // Clang is looking for the type of "*self"
            
            VariableList *vars = m_parser_vars->m_exe_ctx->frame->GetVariableList(false);

            if (!vars)
                return;
        
            lldb::VariableSP self_var = vars->FindVariable(ConstString("self"));
        
            if (!self_var || 
                !self_var->IsInScope(m_parser_vars->m_exe_ctx->frame) || 
                !self_var->LocationIsValidForFrame (m_parser_vars->m_exe_ctx->frame))
                return;
        
            Type *self_type = self_var->GetType();
            
            if (!self_type)
                return;
        
            TypeFromUser self_user_type(self_type->GetClangFullType(),
                                        self_type->GetClangAST());
            
            m_struct_vars->m_object_pointer_type = self_user_type;

            void *pointer_target_type = NULL;
        
            if (!ClangASTContext::IsPointerType(self_user_type.GetOpaqueQualType(),
                                                &pointer_target_type)
                || pointer_target_type == NULL)
                return;
        
            TypeFromUser class_user_type(pointer_target_type,
                                         self_type->GetClangAST());
            
            if (log)
            {
                StreamString type_stream;
                class_user_type.DumpTypeCode(&type_stream);
                type_stream.Flush();
                log->Printf("Adding type for $__lldb_objc_class: %s", type_stream.GetString().c_str());
            }
            
            AddOneType(context, class_user_type, false);
            
            return;
        }

        ClangExpressionVariableSP pvar_sp(m_parser_vars->m_persistent_vars->GetVariable(name));
    
        if (pvar_sp)
        {
            AddOneVariable(context, pvar_sp);
            return;
        }
        
        const char *reg_name(&name.GetCString()[1]);
        
        if (m_parser_vars->m_exe_ctx->GetRegisterContext())
        {
            const RegisterInfo *reg_info(m_parser_vars->m_exe_ctx->GetRegisterContext()->GetRegisterInfoByName(reg_name));
            
            if (reg_info)
                AddOneRegister(context, reg_info);
        }
    }
    
    lldb::TypeSP type_sp (m_parser_vars->m_sym_ctx.FindTypeByName (name));
        
    if (type_sp)
    {
        if (log)
        {
            log->Printf ("Matching type found for \"%s\": ", name.GetCString());
            StreamString strm;
            type_sp->Dump(&strm, true);
            log->PutCString (strm.GetData());
        }

        TypeFromUser user_type (type_sp->GetClangFullType(),
                                type_sp->GetClangAST());
            
        AddOneType(context, user_type, false);
    }
}

clang::ExternalLoadResult
ClangExpressionDeclMap::FindExternalLexicalDecls (const DeclContext *decl_context, 
                                                  bool (*predicate)(Decl::Kind),
                                                  llvm::SmallVectorImpl<Decl*> &decls)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
        
    const Decl *context_decl = dyn_cast<Decl>(decl_context);
    
    if (!context_decl)
        return ELR_Failure;
    
    ASTContext *ast_context = &context_decl->getASTContext();
    
    if (log)
        log->Printf("Finding lexical decls in a '%s' with %s predicate", context_decl->getDeclKindName(), (predicate ? "non-null" : "null"));
    
    Decl *original_decl = NULL;
    ASTContext *original_ctx = NULL;
    
    ClangASTImporter *ast_importer = m_parser_vars->GetASTImporter(ast_context);
    
    if (!ast_importer)
        return ELR_Failure;
    
    if (!ast_importer->ResolveDeclOrigin(context_decl, &original_decl, &original_ctx))
        return ELR_Failure;
    
    if (log)
    {
        std::string decl_print_string;
        llvm::raw_string_ostream decl_print_stream(decl_print_string);
        original_decl->print(decl_print_stream);
        decl_print_stream.flush();
        log->Printf("Original decl:\n%s", decl_print_string.c_str());
    }
    
    if (TagDecl *original_tag_decl = dyn_cast<TagDecl>(original_decl))
    {
        ExternalASTSource *external_source = original_ctx->getExternalSource();
        
        if (!external_source)
            return ELR_Failure;
        
        if (!original_tag_decl)
            return ELR_Failure;
        
        external_source->CompleteType (original_tag_decl);
    }
    
    DeclContext *original_decl_context = dyn_cast<DeclContext>(original_decl);
    
    if (!original_decl_context)
        return ELR_Failure;
    
    for (TagDecl::decl_iterator iter = original_decl_context->decls_begin();
         iter != original_decl_context->decls_end();
         ++iter)
    {
        Decl *decl = *iter;
        
        if (!predicate || predicate(decl->getKind()))
        {
            if (log)
            {
                std::string decl_print_string;
                llvm::raw_string_ostream decl_print_stream(decl_print_string);
                decl->print(decl_print_stream);
                decl_print_stream.flush();
                log->Printf("  Adding lexical decl %s", decl_print_string.c_str());
            }
                        
            Decl *copied_decl = ast_importer->CopyDecl(original_ctx, decl);
            
            decls.push_back(copied_decl);
        }
    }
    
    return ELR_AlreadyLoaded;
}

void
ClangExpressionDeclMap::CompleteTagDecl (TagDecl *tag_decl)
{    
    assert (m_parser_vars.get());
    
    m_parser_vars->GetASTImporter(&tag_decl->getASTContext())->CompleteTagDecl (tag_decl);
}

void
ClangExpressionDeclMap::CompleteObjCInterfaceDecl (clang::ObjCInterfaceDecl *interface_decl)
{
    assert (m_parser_vars.get());
    
    m_parser_vars->GetASTImporter(&interface_decl->getASTContext())->CompleteObjCInterfaceDecl (interface_decl);
}

Value *
ClangExpressionDeclMap::GetVariableValue
(
    ExecutionContext &exe_ctx,
    VariableSP var,
    ASTContext *parser_ast_context,
    TypeFromUser *user_type,
    TypeFromParser *parser_type
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    Type *var_type = var->GetType();
    
    if (!var_type) 
    {
        if (log)
            log->PutCString("Skipped a definition because it has no type");
        return NULL;
    }
    
    clang_type_t var_opaque_type = var_type->GetClangFullType();
    
    if (!var_opaque_type)
    {
        if (log)
            log->PutCString("Skipped a definition because it has no Clang type");
        return NULL;
    }
    
    ASTContext *ast = var_type->GetClangASTContext().getASTContext();
    
    if (!ast)
    {
        if (log)
            log->PutCString("There is no AST context for the current execution context");
        return NULL;
    }
    
    DWARFExpression &var_location_expr = var->LocationExpression();
    
    std::auto_ptr<Value> var_location(new Value);
    
    lldb::addr_t loclist_base_load_addr = LLDB_INVALID_ADDRESS;
    
    if (var_location_expr.IsLocationList())
    {
        SymbolContext var_sc;
        var->CalculateSymbolContext (&var_sc);
        loclist_base_load_addr = var_sc.function->GetAddressRange().GetBaseAddress().GetLoadAddress (exe_ctx.target);
    }
    Error err;
    
    if (!var_location_expr.Evaluate(&exe_ctx, ast, NULL, NULL, NULL, loclist_base_load_addr, NULL, *var_location.get(), &err))
    {
        if (log)
            log->Printf("Error evaluating location: %s", err.AsCString());
        return NULL;
    }
        
    void *type_to_use = NULL;
    
    if (parser_ast_context)
    {
        type_to_use = GuardedCopyType(parser_ast_context, ast, var_opaque_type);
        
        if (!type_to_use)
        {
            if (log)
                log->Printf("Couldn't copy a variable's type into the parser's AST context");
            
            return NULL;
        }
        
        if (parser_type)
            *parser_type = TypeFromParser(type_to_use, parser_ast_context);
    }
    else
        type_to_use = var_opaque_type;
    
    if (var_location.get()->GetContextType() == Value::eContextTypeInvalid)
        var_location.get()->SetContext(Value::eContextTypeClangType, type_to_use);
    
    if (var_location.get()->GetValueType() == Value::eValueTypeFileAddress)
    {
        SymbolContext var_sc;
        var->CalculateSymbolContext(&var_sc);
        
        if (!var_sc.module_sp)
            return NULL;
        
        ObjectFile *object_file = var_sc.module_sp->GetObjectFile();
        
        if (!object_file)
            return NULL;
        
        Address so_addr(var_location->GetScalar().ULongLong(), object_file->GetSectionList());
        
        lldb::addr_t load_addr = so_addr.GetLoadAddress(exe_ctx.target);
        
        var_location->GetScalar() = load_addr;
        var_location->SetValueType(Value::eValueTypeLoadAddress);
    }
    
    if (user_type)
        *user_type = TypeFromUser(var_opaque_type, ast);
    
    return var_location.release();
}

void
ClangExpressionDeclMap::AddOneVariable (NameSearchContext &context, VariableSP var)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    TypeFromUser ut;
    TypeFromParser pt;
    
    Value *var_location = GetVariableValue (*m_parser_vars->m_exe_ctx, 
                                            var, 
                                            context.GetASTContext(),
                                            &ut,
                                            &pt);
    
    if (!var_location)
        return;
    
    NamedDecl *var_decl = context.AddVarDecl(ClangASTContext::CreateLValueReferenceType(pt.GetASTContext(), pt.GetOpaqueQualType()));
    std::string decl_name(context.m_decl_name.getAsString());
    ConstString entity_name(decl_name.c_str());
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      entity_name, 
                                                                      ut,
                                                                      m_parser_vars->m_exe_ctx->process->GetByteOrder(),
                                                                      m_parser_vars->m_exe_ctx->process->GetAddressByteSize()));
    assert (entity.get());
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = pt;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = var_location;
    entity->m_parser_vars->m_lldb_var    = var;
    
    if (log)
    {
        std::string var_decl_print_string;
        llvm::raw_string_ostream var_decl_print_stream(var_decl_print_string);
        var_decl->print(var_decl_print_stream);
        var_decl_print_stream.flush();
        
        log->Printf("Found variable %s, returned %s", decl_name.c_str(), var_decl_print_string.c_str());

        //if (log->GetVerbose())
        //{
        //    StreamString var_decl_dump_string;
        //    ASTDumper::DumpDecl(var_decl_dump_string, var_decl);
        //    log->Printf("%s\n", var_decl_dump_string.GetData());
        //}
    }
}

void
ClangExpressionDeclMap::AddOneVariable(NameSearchContext &context,
                                       ClangExpressionVariableSP &pvar_sp)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    TypeFromUser user_type (pvar_sp->GetTypeFromUser());
    
    TypeFromParser parser_type (GuardedCopyType(context.GetASTContext(), 
                                                user_type.GetASTContext(), 
                                                user_type.GetOpaqueQualType()),
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(ClangASTContext::CreateLValueReferenceType(parser_type.GetASTContext(), parser_type.GetOpaqueQualType()));
    
    pvar_sp->EnableParserVars();
    pvar_sp->m_parser_vars->m_parser_type = parser_type;
    pvar_sp->m_parser_vars->m_named_decl  = var_decl;
    pvar_sp->m_parser_vars->m_llvm_value  = NULL;
    pvar_sp->m_parser_vars->m_lldb_value  = NULL;
    
    if (log)
    {
        std::string var_decl_print_string;
        llvm::raw_string_ostream var_decl_print_stream(var_decl_print_string);
        var_decl->print(var_decl_print_stream);
        var_decl_print_stream.flush();
        
        log->Printf("Added pvar %s, returned %s", pvar_sp->GetName().GetCString(), var_decl_print_string.c_str());
    }
}

void
ClangExpressionDeclMap::AddOneGenericVariable(NameSearchContext &context, 
                                              Symbol &symbol)
{
    assert(m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ASTContext *scratch_ast_context = m_parser_vars->m_exe_ctx->target->GetScratchClangASTContext()->getASTContext();
    
    TypeFromUser user_type (ClangASTContext::CreateLValueReferenceType(scratch_ast_context, ClangASTContext::GetVoidPtrType(scratch_ast_context, true)),
                            scratch_ast_context);
    
    TypeFromParser parser_type (ClangASTContext::CreateLValueReferenceType(scratch_ast_context, ClangASTContext::GetVoidPtrType(context.GetASTContext(), true)),
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(parser_type.GetOpaqueQualType());
    
    std::string decl_name(context.m_decl_name.getAsString());
    ConstString entity_name(decl_name.c_str());
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      entity_name, 
                                                                      user_type,
                                                                      m_parser_vars->m_exe_ctx->process->GetByteOrder(),
                                                                      m_parser_vars->m_exe_ctx->process->GetAddressByteSize()));
    assert (entity.get());
    
    std::auto_ptr<Value> symbol_location(new Value);
    
    AddressRange &symbol_range = symbol.GetAddressRangeRef();
    Address &symbol_address = symbol_range.GetBaseAddress();
    lldb::addr_t symbol_load_addr = symbol_address.GetLoadAddress(m_parser_vars->m_exe_ctx->target);
    
    symbol_location->SetContext(Value::eContextTypeClangType, user_type.GetOpaqueQualType());
    symbol_location->GetScalar() = symbol_load_addr;
    symbol_location->SetValueType(Value::eValueTypeLoadAddress);
    
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = parser_type;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = symbol_location.release();
    entity->m_parser_vars->m_lldb_sym    = &symbol;
    //entity->m_flags                      |= ClangExpressionVariable::EVUnknownType;
    
    if (log)
    {
        std::string var_decl_print_string;
        llvm::raw_string_ostream var_decl_print_stream(var_decl_print_string);
        var_decl->print(var_decl_print_stream);
        var_decl_print_stream.flush();
        
        log->Printf("Found variable %s, returned %s", decl_name.c_str(), var_decl_print_string.c_str());
        
        //if (log->GetVerbose())
        //{
        //    StreamString var_decl_dump_string;
        //    ASTDumper::DumpDecl(var_decl_dump_string, var_decl);
        //    log->Printf("%s\n", var_decl_dump_string.GetData());
        //}
    }
}

bool 
ClangExpressionDeclMap::ResolveUnknownTypes()
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ASTContext *scratch_ast_context = m_parser_vars->m_exe_ctx->target->GetScratchClangASTContext()->getASTContext();

    for (size_t index = 0, num_entities = m_found_entities.GetSize();
         index < num_entities;
         ++index)
    {
        ClangExpressionVariableSP entity = m_found_entities.GetVariableAtIndex(index);
        
        if (entity->m_flags & ClangExpressionVariable::EVUnknownType)
        {
            const NamedDecl *named_decl = entity->m_parser_vars->m_named_decl;
            const VarDecl *var_decl = dyn_cast<VarDecl>(named_decl);
            
            if (!var_decl)
            {
                if (log)
                    log->Printf("Entity of unknown type does not have a VarDecl");
                return false;
            }
            
            if (log)
            {
                std::string var_decl_print_string;
                llvm::raw_string_ostream var_decl_print_stream(var_decl_print_string);
                var_decl->print(var_decl_print_stream);
                var_decl_print_stream.flush();
            
                log->Printf("Variable of unknown type now has Decl %s", var_decl_print_string.c_str());
            }
                
            QualType var_type = var_decl->getType();
            TypeFromParser parser_type(var_type.getAsOpaquePtr(), &var_decl->getASTContext());
            
            lldb::clang_type_t copied_type = ClangASTContext::CopyType(scratch_ast_context, &var_decl->getASTContext(), var_type.getAsOpaquePtr());
            
            TypeFromUser user_type(copied_type, scratch_ast_context);
                        
            entity->m_parser_vars->m_lldb_value->SetContext(Value::eContextTypeClangType, user_type.GetOpaqueQualType());
            entity->m_parser_vars->m_parser_type = parser_type;
            
            entity->SetClangAST(user_type.GetASTContext());
            entity->SetClangType(user_type.GetOpaqueQualType());
            
            entity->m_flags &= ~(ClangExpressionVariable::EVUnknownType);
        }
    }
            
    return true;
}

void
ClangExpressionDeclMap::AddOneRegister (NameSearchContext &context,
                                        const RegisterInfo *reg_info)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    void *ast_type = ClangASTContext::GetBuiltinTypeForEncodingAndBitSize(context.GetASTContext(),
                                                                          reg_info->encoding,
                                                                          reg_info->byte_size * 8);
    
    if (!ast_type)
    {
        if (log)
            log->Printf("Tried to add a type for %s, but couldn't get one", context.m_decl_name.getAsString().c_str());
        return;
    }
    
    TypeFromParser parser_type (ast_type,
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(parser_type.GetOpaqueQualType());
    
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope(),
                                                                      m_parser_vars->m_exe_ctx->process->GetByteOrder(),
                                                                      m_parser_vars->m_exe_ctx->process->GetAddressByteSize()));
    assert (entity.get());
    std::string decl_name(context.m_decl_name.getAsString());
    entity->SetName (ConstString (decl_name.c_str()));
    entity->SetRegisterInfo (reg_info);
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = parser_type;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = NULL;
    
    if (log)
    {
        std::string var_decl_print_string;
        llvm::raw_string_ostream var_decl_print_stream(var_decl_print_string);
        var_decl->print(var_decl_print_stream);
        var_decl_print_stream.flush();
        
        log->Printf("Added register %s, returned %s", context.m_decl_name.getAsString().c_str(), var_decl_print_string.c_str());
    }
}

NamespaceDecl *
ClangExpressionDeclMap::AddNamespace (NameSearchContext &context, const ClangNamespaceDecl &namespace_decl)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    assert (m_parser_vars.get());
    
    Decl *copied_decl = m_parser_vars->GetASTImporter(context.GetASTContext())->CopyDecl(namespace_decl.GetASTContext(), 
                                                                                                namespace_decl.GetNamespaceDecl());

    return dyn_cast<NamespaceDecl>(copied_decl);
}

void
ClangExpressionDeclMap::AddOneFunction (NameSearchContext &context,
                                        Function* fun,
                                        Symbol* symbol)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    NamedDecl *fun_decl = NULL;
    std::auto_ptr<Value> fun_location(new Value);
    const Address *fun_address = NULL;
    
    // only valid for Functions, not for Symbols
    void *fun_opaque_type = NULL;
    ASTContext *fun_ast_context = NULL;
    
    if (fun)
    {
        Type *fun_type = fun->GetType();
        
        if (!fun_type) 
        {
            if (log)
                log->PutCString("Skipped a function because it has no type");
            return;
        }
        
        fun_opaque_type = fun_type->GetClangFullType();
        
        if (!fun_opaque_type)
        {
            if (log)
                log->PutCString("Skipped a function because it has no Clang type");
            return;
        }
        
        fun_address = &fun->GetAddressRange().GetBaseAddress();
        
        fun_ast_context = fun_type->GetClangASTContext().getASTContext();
        void *copied_type = GuardedCopyType(context.GetASTContext(), fun_ast_context, fun_opaque_type);
        
        fun_decl = context.AddFunDecl(copied_type);
    }
    else if (symbol)
    {
        fun_address = &symbol->GetAddressRangeRef().GetBaseAddress();
        
        fun_decl = context.AddGenericFunDecl();
    }
    else
    {
        if (log)
            log->PutCString("AddOneFunction called with no function and no symbol");
        return;
    }
    
    lldb::addr_t load_addr = fun_address->GetCallableLoadAddress(m_parser_vars->m_exe_ctx->target);
    fun_location->SetValueType(Value::eValueTypeLoadAddress);
    fun_location->GetScalar() = load_addr;
    
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      m_parser_vars->m_exe_ctx->process->GetByteOrder(),
                                                                      m_parser_vars->m_exe_ctx->process->GetAddressByteSize()));
    assert (entity.get());
    std::string decl_name(context.m_decl_name.getAsString());
    entity->SetName(ConstString(decl_name.c_str()));
    entity->SetClangType (fun_opaque_type);
    entity->SetClangAST (fun_ast_context);
    
    entity->EnableParserVars();
    entity->m_parser_vars->m_named_decl  = fun_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = fun_location.release();
        
    if (log)
    {
        std::string fun_decl_print_string;
        llvm::raw_string_ostream fun_decl_print_stream(fun_decl_print_string);
        fun_decl->print(fun_decl_print_stream);
        fun_decl_print_stream.flush();
        
        log->Printf("Found %s function %s, returned %s", (fun ? "specific" : "generic"), decl_name.c_str(), fun_decl_print_string.c_str());
    }
}

void 
ClangExpressionDeclMap::AddOneType(NameSearchContext &context, 
                                   TypeFromUser &ut,
                                   bool add_method)
{
    ASTContext *parser_ast_context = context.GetASTContext();
    ASTContext *user_ast_context = ut.GetASTContext();
    
    void *copied_type = GuardedCopyType(parser_ast_context, user_ast_context, ut.GetOpaqueQualType());
 
    TypeFromParser parser_type(copied_type, parser_ast_context);
    
    if (add_method && ClangASTContext::IsAggregateType(copied_type))
    {
        void *args[1];
        
        args[0] = ClangASTContext::GetVoidPtrType(parser_ast_context, false);
        
        void *method_type = ClangASTContext::CreateFunctionType (parser_ast_context,
                                                                 ClangASTContext::GetBuiltInType_void(parser_ast_context),
                                                                 args,
                                                                 1,
                                                                 false,
                                                                 ClangASTContext::GetTypeQualifiers(copied_type));

        const bool is_virtual = false;
        const bool is_static = false;
        const bool is_inline = false;
        const bool is_explicit = false;
        
        ClangASTContext::AddMethodToCXXRecordType (parser_ast_context,
                                                   copied_type,
                                                   "$__lldb_expr",
                                                   method_type,
                                                   lldb::eAccessPublic,
                                                   is_virtual,
                                                   is_static,
                                                   is_inline,
                                                   is_explicit);
    }
    
    context.AddTypeDecl(copied_type);
}

void * 
ClangExpressionDeclMap::GuardedCopyType (ASTContext *dest_context, 
                                         ASTContext *source_context,
                                         void *clang_type)
{
    assert (m_parser_vars.get());
    
    m_parser_vars->m_ignore_lookups = true;
    
    lldb_private::ClangASTImporter *importer = m_parser_vars->GetASTImporter(dest_context);
    
    QualType ret_qual_type = importer->CopyType (source_context,
                                                 QualType::getFromOpaquePtr(clang_type));
    
    void *ret = ret_qual_type.getAsOpaquePtr();
    
    m_parser_vars->m_ignore_lookups = false;
    
    return ret;
}
