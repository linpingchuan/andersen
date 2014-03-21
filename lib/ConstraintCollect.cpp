#include "Andersen.h"
#include "Helper.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// CollectConstraints - This stage scans the program, adding a constraint to the Constraints list for each instruction in the program that induces a constraint, and setting up the initial points-to graph.

void Andersen::collectConstraints(Module& M)
{
	// First, the universal set points to itself.
	constraints.emplace_back(AndersConstraint::ADDR_OF,
		nodeFactory.getUniversalPtrNode(), nodeFactory.getUniversalObjNode());
	constraints.emplace_back(AndersConstraint::STORE,
		nodeFactory.getUniversalObjNode(), nodeFactory.getUniversalObjNode());

	// Next, the null pointer points to the null object.
	constraints.emplace_back(AndersConstraint::ADDR_OF,
		nodeFactory.getNullPtrNode(), nodeFactory.getNullObjectNode());

	// Before we start, collect the struct information in the input program for field-sensitive analysis
	structAnalyzer.run(M);
	nodeFactory.setStructAnalyzer(&structAnalyzer);

	// Next, add any constraints on global variables. Associate the address of the global object as pointing to the memory for the global: &G = <G memory>
	collectConstraintsForGlobals(M);

	// Here is a notable points before we proceed:
	// For functions with non-local linkage type, theoretically we should not trust anything that get passed to it or get returned by it. However, precision will be seriously hurt if we do that because if we do not run a -internalize pass before the -anders pass, almost every function is marked external. We'll just assume that even external linkage will not ruin the analysis result first

	for (auto const& f: M)
	{
		if (f.isDeclaration() || f.isIntrinsic())
			continue;

		if (isa<PointerType>(f.getFunctionType()->getReturnType()))
			nodeFactory.createReturnNode(&f);

		if (f.getFunctionType()->isVarArg())
			nodeFactory.createVarargNode(&f);

		// Add nodes for all formal arguments.
		for (Function::const_arg_iterator itr = f.arg_begin(), ite = f.arg_end(); itr != ite; ++itr)
		{
			if (isa<PointerType>(itr->getType()))
				nodeFactory.createValueNode(itr);
		}

		// Scan the function body
		// A visitor pattern might help modularity, but it needs more boilerplate codes to set up, and it breaks down the main logic into pieces 

		// First, create a value node for each instructino with pointer type. It is necessary to do the job here rather than on-the-fly because an instruction may refer to the value node definied before it (e.g. phi nodes)
		for (const_inst_iterator itr = inst_begin(f), ite = inst_end(f); itr != ite; ++itr)
		{
			const Instruction* inst = itr.getInstructionIterator();
			if (inst->getType()->isPointerTy())
				nodeFactory.createValueNode(inst);
		}

		// Now, collect constraint for each relevant instruction
		for (const_inst_iterator itr = inst_begin(f), ite = inst_end(f); itr != ite; ++itr)
		{
			const Instruction* inst = itr.getInstructionIterator();
			collectConstraintsForInstruction(inst);
		}
	}
}

// This function performs similar task to llvm::isAllocationFn (without the prototype check). However, the llvm version does not correctly handle functions like memalign and posix_memalign, and that is why we have to re-write it here again...
static bool isMallocCall(ImmutableCallSite cs, const TargetLibraryInfo* tli)
{
	const Function* callee = cs.getCalledFunction();
	if (callee == nullptr || !callee->isDeclaration())
		return false;

	static const LibFunc::Func AllocationFns[] = {
		LibFunc::malloc, LibFunc::valloc, LibFunc::calloc, LibFunc::realloc, LibFunc::reallocf, 
		LibFunc::Znwj, LibFunc::ZnwjRKSt9nothrow_t,
		LibFunc::Znwm, LibFunc::ZnwmRKSt9nothrow_t, 
		LibFunc::Znaj, LibFunc::ZnajRKSt9nothrow_t, 
		LibFunc::Znam, LibFunc::ZnamRKSt9nothrow_t, 
		LibFunc::strdup, LibFunc::strndup,
		LibFunc::memalign, LibFunc::posix_memalign 
	};

	StringRef fName = callee->getName();
	LibFunc::Func tliFunc;
	if (tli == NULL || !tli->getLibFunc(fName, tliFunc))
		return false;

	for (unsigned i = 0, e = array_lengthof(AllocationFns); i < e; ++i)
	{
		if (AllocationFns[i] == tliFunc)
			return true;
	}

	// TODO: check prototype
	return false;
}

void Andersen::collectConstraintsForGlobals(Module& M)
{
	for (auto const& globalVal: M.globals())
	{
		const Type *type = globalVal.getType()->getElementType();
		
		// An array is considered a single variable of its type.
		while(const ArrayType *arrayType= dyn_cast<ArrayType>(type))
			type = arrayType->getElementType();

		// Now construct the pointer and memory object variable
		// It depends on whether the type of this variable is a struct or not
		if (const StructType *structType = dyn_cast<StructType>(type))
		{
			// Construct a stuctVar for the entire variable
			processStruct(&globalVal, structType);
		}
		else
		{
			NodeIndex gVal = nodeFactory.createValueNode(&globalVal);
			NodeIndex gObj = nodeFactory.createObjectNode(&globalVal);
			constraints.emplace_back(AndersConstraint::ADDR_OF, gVal, gObj);
		}
	}

	// Functions and function pointers are also considered global
	for (auto const& f: M)
	{
		// If f is an addr-taken function, create a pointer and an object for it
		if (f.hasAddressTaken())
		{
			NodeIndex fVal = nodeFactory.createValueNode(&f);
			NodeIndex fObj = nodeFactory.createObjectNode(&f);
			constraints.emplace_back(AndersConstraint::ADDR_OF, fVal, fObj);
		}
	}

	// Init globals here since an initializer may refer to a global var/func below it
	for (auto const& globalVal: M.globals())
	{
		NodeIndex gObj = nodeFactory.getObjectNodeFor(&globalVal);
		if (gObj == AndersNodeFactory::InvalidIndex)	// Empty struct
			continue;

		if (globalVal.hasDefinitiveInitializer())
		{
			addGlobalInitializerConstraints(gObj, globalVal.getInitializer());
		}
		else
		{
			// If it doesn't have an initializer (i.e. it's defined in another
			// translation unit), it points to the universal set.
			constraints.emplace_back(AndersConstraint::COPY,
				gObj, nodeFactory.getUniversalObjNode());
		}
	}
}

void Andersen::processStruct(const Value* v, const StructType* stType)
{
	// We cannot handle opaque type
	assert(!stType->isOpaque() && "Opaque type not supported");
	// Sanity check
	assert(stType != NULL && "structType is NULL");

	const StructInfo* stInfo = structAnalyzer.getStructInfo(stType);
	assert(stInfo != NULL && "structInfoMap should have info for all structs!");

	// Empty struct has only one pointer that points to nothing
	if (stInfo->isEmpty())
	{
		NodeIndex ptr = nodeFactory.createValueNode(v);
		constraints.emplace_back(AndersConstraint::ADDR_OF, ptr, nodeFactory.getNullObjectNode());
		return;
	}

	// Non-empty structs: create one pointer and one target for each field
	unsigned stSize = stInfo->getExpandedSize();
	// We only need to construct a single top-level variable that points to the starting location. Pointers to locations that follow are not visible on LLVM IR level
	NodeIndex ptr = nodeFactory.createValueNode(v);
	
	// We construct a target variable for each field
	// A better approach is to collect all constant GEP instructions and construct variables only if they are used. We want to do the simplest thing first
	NodeIndex obj = nodeFactory.createObjectNode(v);
	for (unsigned i = 1; i < stSize; ++i)
		nodeFactory.createObjectNode();
	
	constraints.emplace_back(AndersConstraint::ADDR_OF, ptr, obj);
}

void Andersen::addGlobalInitializerConstraints(NodeIndex objNode, const Constant* c)
{
	//errs() << "Called with node# = " << objNode << ", initializer = " << *c << "\n";
	if (c->getType()->isSingleValueType())
	{
		if (isa<PointerType>(c->getType()))
		{
			NodeIndex rhsNode = nodeFactory.getObjectNodeForConstant(c);
			assert(rhsNode != AndersNodeFactory::InvalidIndex && "rhs node not found");
			constraints.emplace_back(AndersConstraint::ADDR_OF, objNode, rhsNode);
		}
	}
	else if (c->isNullValue())
	{
		constraints.emplace_back(AndersConstraint::COPY, objNode, nodeFactory.getNullObjectNode());
	}
	else if (!isa<UndefValue>(c))
	{
		// If this is an array, include constraints for each element.
		if (isa<ConstantArray>(c) || isa<ConstantDataSequential>(c))
		{
			for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i)
				addGlobalInitializerConstraints(objNode, cast<Constant>(c->getOperand(i)));
		}
		else if (isa<ConstantStruct>(c))
		{
			StructType* stType = cast<StructType>(c->getType());
			const StructInfo* stInfo = structAnalyzer.getStructInfo(stType);
			assert(stInfo != NULL && "structInfoMap should have info for all structs!");
			
			// Sequentially initialize each field
			for (unsigned i = 0; i < c->getNumOperands(); ++i)
			{
				NodeIndex field = nodeFactory.getOffsetObjectNode(objNode, i);
				Constant* cv = cast<Constant>(c->getOperand(i));
				addGlobalInitializerConstraints(field, cv);
			}
		}
		else
			llvm_unreachable("Unexpected global initializer");
	}
}

static unsigned getGEPInstFieldNum(const GetElementPtrInst* gepInst, const DataLayout* dataLayout, const StructAnalyzer& structAnalyzer)
{
	unsigned offset= getGEPOffset(gepInst, dataLayout);

	const Value* ptr = GetUnderlyingObject(gepInst->getPointerOperand(), dataLayout, 0);
	Type* trueElemType = cast<PointerType>(ptr->getType())->getElementType();

	unsigned ret = 0;
	while (offset > 0)
	{
		// Collapse array type
		while(const ArrayType *arrayType= dyn_cast<ArrayType>(trueElemType))
			trueElemType = arrayType->getElementType();

		//errs() << "trueElemType = "; trueElemType->dump(); errs() << "\n";
		offset %= dataLayout->getTypeAllocSize(trueElemType);
		if (trueElemType->isStructTy())
		{
			StructType* stType = cast<StructType>(trueElemType);
			const StructLayout* stLayout = dataLayout->getStructLayout(stType);
			unsigned idx = stLayout->getElementContainingOffset(offset);
			const StructInfo* stInfo = structAnalyzer.getStructInfo(stType);
			assert(stInfo != NULL && "structInfoMap should have info for all structs!");
			
			ret += stInfo->getOffset(idx);
			offset -= stLayout->getElementOffset(idx);
			trueElemType = stType->getElementType(idx);
		}
		else
		{
			if (offset != 0)
			{
				errs() << "Warning: GEP into the middle of a field. This usually occurs when union is used. Since partial alias is not supported, correctness is not guanranteed here.\n";
				break;
			}
		}
	}
	return ret;
}

void Andersen::collectConstraintsForInstruction(const Instruction* inst)
{
	switch (inst->getOpcode())
	{
		case Instruction::Alloca:
		{
			NodeIndex valNode = nodeFactory.createValueNode(inst);
			NodeIndex objNode = nodeFactory.createObjectNode(inst);
			constraints.emplace_back(AndersConstraint::ADDR_OF, valNode, objNode);
			break;
		}
		case Instruction::Call:
		case Instruction::Invoke:
		{
			ImmutableCallSite cs(inst);
			assert(cs && "Something wrong with callsite?");

			if (isMallocCall(cs, tli))
			{
				NodeIndex ptrIndex = nodeFactory.getValueNodeFor(inst);
				assert(ptrIndex != AndersNodeFactory::InvalidIndex && "Failed to find malloc-call value node");
				NodeIndex objIndex = nodeFactory.createObjectNode(inst);
				constraints.emplace_back(AndersConstraint::ADDR_OF, ptrIndex, objIndex);
				return;
			}

			addConstraintForCall(cs);

			break;
		}
		case Instruction::Ret:
		{
			if (inst->getNumOperands() > 0 && inst->getOperand(0)->getType()->isPointerTy())
			{
				NodeIndex retIndex = nodeFactory.getReturnNodeFor(inst->getParent()->getParent());
				assert(retIndex != AndersNodeFactory::InvalidIndex && "Failed to find return node");
				NodeIndex valIndex = nodeFactory.getValueNodeFor(inst->getOperand(0));
				assert(valIndex != AndersNodeFactory::InvalidIndex && "Failed to find return value node");
				constraints.emplace_back(AndersConstraint::COPY, retIndex, valIndex);
			}
			break;
		}
		case Instruction::Load:
		{
			if (inst->getType()->isPointerTy())
			{
				NodeIndex opIndex = nodeFactory.getValueNodeFor(inst->getOperand(0));
				assert(opIndex != AndersNodeFactory::InvalidIndex && "Failed to find load operand node");
				NodeIndex valIndex = nodeFactory.getValueNodeFor(inst);
				assert(valIndex != AndersNodeFactory::InvalidIndex && "Failed to find load value node");
				constraints.emplace_back(AndersConstraint::LOAD, valIndex, opIndex);
			}
			break;
		}
		case Instruction::Store:
		{
			if (inst->getType()->isPointerTy())
			{
				NodeIndex srcIndex = nodeFactory.getValueNodeFor(inst->getOperand(0));
				assert(srcIndex != AndersNodeFactory::InvalidIndex && "Failed to find store src node");
				NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst->getOperand(1));
				assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find store dst node");
				constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
			}
			break;
		}
		case Instruction::GetElementPtr:
		{
			assert(inst->getType()->isPointerTy());

			NodeIndex srcIndex = nodeFactory.getValueNodeFor(inst->getOperand(0));
			assert(srcIndex != AndersNodeFactory::InvalidIndex && "Failed to find gep src node");
			NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst);
			assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find gep dst node");

			unsigned fieldNum = getGEPInstFieldNum(cast<GetElementPtrInst>(inst), dataLayout, structAnalyzer);
			constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex, fieldNum);

			break;
		}
		case Instruction::PHI:
		{
			if (inst->getType()->isPointerTy())
			{
				const PHINode* phiInst = cast<PHINode>(inst);
				NodeIndex dstIndex = nodeFactory.getValueNodeFor(phiInst);
				assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find phi dst node");
				for (unsigned i = 0, e = phiInst->getNumIncomingValues(); i != e; ++i)
				{
					NodeIndex srcIndex = nodeFactory.getValueNodeFor(phiInst->getIncomingValue(i));
					assert(srcIndex != AndersNodeFactory::InvalidIndex && "Failed to find phi src node");
					constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
				}
			}
			break;
		}
		case Instruction::BitCast:
		{
			if (inst->getType()->isPointerTy())
			{
				NodeIndex srcIndex = nodeFactory.getValueNodeFor(inst->getOperand(0));
				assert(srcIndex != AndersNodeFactory::InvalidIndex && "Failed to find bitcast src node");
				NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst);
				assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find bitcast dst node");
				constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
			}
			break;
		}
		case Instruction::IntToPtr:
		{
			assert(inst->getType()->isPointerTy());
			NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst);
			assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find inttoptr dst node");
			constraints.emplace_back(AndersConstraint::COPY, dstIndex, nodeFactory.getUniversalPtrNode());
			break;
		}
		case Instruction::Select:
		{
			if (inst->getType()->isPointerTy())
			{
				NodeIndex srcIndex1 = nodeFactory.getValueNodeFor(inst->getOperand(1));
				assert(srcIndex1 != AndersNodeFactory::InvalidIndex && "Failed to find select src node 1");
				NodeIndex srcIndex2 = nodeFactory.getValueNodeFor(inst->getOperand(2));
				assert(srcIndex2 != AndersNodeFactory::InvalidIndex && "Failed to find select src node 2");
				NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst);
				assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find select dst node");
				constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex1);
				constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex2);
			}
			break;
		}
		case Instruction::VAArg:
		{
			if (inst->getType()->isPointerTy())
			{
				NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst);
				assert(dstIndex != AndersNodeFactory::InvalidIndex && "Failed to find va_arg dst node");
				NodeIndex vaIndex = nodeFactory.getVarargNodeFor(inst->getParent()->getParent());
				assert(vaIndex != AndersNodeFactory::InvalidIndex && "Failed to find vararg node");
				constraints.emplace_back(AndersConstraint::COPY, dstIndex, vaIndex);
			}
			break;
		}
		// We should rely on a preliminary pass to translates extractvalue/insertvalue into GEPs
		case Instruction::ExtractValue:
		case Instruction::InsertValue:
		// We have no intention to support exception-handling in the near future
		case Instruction::LandingPad:
		case Instruction::Resume:
		// Atomic instructions can be modeled by their non-atomic counterparts. To be supported
		case Instruction::AtomicRMW:
		case Instruction::AtomicCmpXchg:
		{
			assert(false && "not implemented yet");
		}
		default:
		{
			assert(!inst->getType()->isPointerTy() && "pointer-related inst not handled!");
			break;
		}
	}
}

// There are two types of constraints to add for a function call:
// - ValueNode(callsite) = ReturnNode(call target)
// - ValueNode(formal arg) = ValueNode(actual arg)
void Andersen::addConstraintForCall(ImmutableCallSite cs)
{
	if (const Function* f = cs.getCalledFunction())	// Direct call
	{
		if (f->isDeclaration() || f->isIntrinsic())	// External library call
		{
			// Handle libraries separately
			if (addConstraintForExternalLibrary(cs))
				return;
			else	// Unresolved library call: ruin everything!
			{
				errs() << "Unresolved ext function: " << f->getName() << "\n";
				if (cs.getType()->isPointerTy())
				{
					NodeIndex retIndex = nodeFactory.getValueNodeFor(cs.getInstruction());
					assert(retIndex != AndersNodeFactory::InvalidIndex && "Failed to find ret node!");
					constraints.emplace_back(AndersConstraint::COPY, retIndex, nodeFactory.getUniversalPtrNode());
				}
				for (ImmutableCallSite::arg_iterator itr = cs.arg_begin(), ite = cs.arg_end(); itr != ite; ++itr)
				{
					NodeIndex argIndex = nodeFactory.getValueNodeFor(*itr);
					assert(argIndex != AndersNodeFactory::InvalidIndex && "Failed to find arg node!");
					constraints.emplace_back(AndersConstraint::COPY, argIndex, nodeFactory.getUniversalPtrNode());
				}
			}
		}
		else	// Non-external function call
		{
			if (cs.getType()->isPointerTy())
			{
				NodeIndex retIndex = nodeFactory.getValueNodeFor(cs.getInstruction());
				assert(retIndex != AndersNodeFactory::InvalidIndex && "Failed to find ret node!");
				NodeIndex fRetIndex = nodeFactory.getReturnNodeFor(f);
				assert(fRetIndex != AndersNodeFactory::InvalidIndex && "Failed to find function ret node!");
				constraints.emplace_back(AndersConstraint::COPY, retIndex, fRetIndex);
			}
		}

		// The argument constraints
		addArgumentConstraintForCall(cs, f);
	}
	else	// Indirect call
	{
		// We do the simplest thing here: just assume the returned value can be anything :)
		if (cs.getType()->isPointerTy())
		{
			NodeIndex retIndex = nodeFactory.getValueNodeFor(cs.getInstruction());
			assert(retIndex != AndersNodeFactory::InvalidIndex && "Failed to find ret node!");
			constraints.emplace_back(AndersConstraint::COPY, retIndex, nodeFactory.getUniversalPtrNode());
		}

		// For argument constraints, first search through all addr-taken functions: any function that takes can take as many variables is a potential candidate
		const Module* M = cs.getInstruction()->getParent()->getParent()->getParent();
		for (auto const& f: *M)
		{
			NodeIndex funPtrIndex = nodeFactory.getValueNodeFor(&f);
			if (funPtrIndex == AndersNodeFactory::InvalidIndex)
				// Not an addr-taken function
				continue;

			if (!f.getFunctionType()->isVarArg() && f.arg_size() != cs.arg_size())
				// #arg mismatch
				continue;

			addArgumentConstraintForCall(cs, &f);
		}
	}
}

void Andersen::addArgumentConstraintForCall(ImmutableCallSite cs, const Function* f)
{
	Function::const_arg_iterator fItr = f->arg_begin();
	ImmutableCallSite::arg_iterator aItr = cs.arg_begin();
	while (fItr != f->arg_end() && aItr != cs.arg_end())
	{
		const Argument* formal = fItr;
		const Value* actual = *aItr;

		if (formal->getType()->isPointerTy())
		{
			NodeIndex fIndex = nodeFactory.getValueNodeFor(formal);
			assert(fIndex != AndersNodeFactory::InvalidIndex && "Failed to find formal arg node!");
			if (actual->getType()->isPointerTy())
			{
				NodeIndex aIndex = nodeFactory.getValueNodeFor(actual);
				assert(aIndex != AndersNodeFactory::InvalidIndex && "Failed to find actual arg node!");
				constraints.emplace_back(AndersConstraint::COPY, fIndex, aIndex);
			}
			else
				constraints.emplace_back(AndersConstraint::COPY, fIndex, nodeFactory.getUniversalPtrNode());
		}

		++fItr, ++aItr;
	}

	// Copy all pointers passed through the varargs section to the varargs node
	if (f->getFunctionType()->isVarArg())
	{
		while (aItr != cs.arg_end())
		{
			const Value* actual = *aItr;
			if (actual->getType()->isPointerTy())
			{
				NodeIndex aIndex = nodeFactory.getValueNodeFor(actual);
				assert(aIndex != AndersNodeFactory::InvalidIndex && "Failed to find actual arg node!");
				NodeIndex vaIndex = nodeFactory.getVarargNodeFor(f);
				assert(vaIndex != AndersNodeFactory::InvalidIndex && "Failed to find vararg node!");
				constraints.emplace_back(AndersConstraint::COPY, vaIndex, aIndex);
			}

			++aItr;
		}
	}
}
