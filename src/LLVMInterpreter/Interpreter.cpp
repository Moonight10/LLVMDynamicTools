#include "Interpreter.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm_interpreter;

Interpreter::Interpreter(llvm::Module* m): module(m), dataLayout(m)
{
}

Interpreter::~Interpreter() {}

Address Interpreter::allocateStackMem(StackFrame& frame, unsigned size)
{
	frame.increaseAllocationSize(size);
	return stackMem.allocate(size);
}

Address Interpreter::allocateGlobalMem(const llvm::GlobalValue* gv)
{
	auto globalType = gv->getType();
	if (globalType->isVectorTy())
		llvm_unreachable("Vector type not supported");

	auto globalSize = dataLayout.getTypeAllocSize(globalType);
	return globalMem.allocate(globalSize);
}

void Interpreter::evaluateGlobals()
{
	globalMem.clear();

	for (auto const& globalVal: module->globals())
	{
		auto globalAddr = allocateGlobalMem(&globalVal);
		globalEnv.insert(std::make_pair(&globalVal, globalAddr));
	}

	for (auto const& globalVal: module->globals())
	{
		auto globalAddr = globalEnv.at(&globalVal);
		if (globalVal.hasInitializer())
			globalMem.write(globalAddr, evaluateConstant(globalVal.getInitializer()));
	}

	// Give each function a corresponding pointer
	for (auto const& f: *module)
	{
		auto funAddr = allocateGlobalMem(&f);
		globalEnv.insert(std::make_pair(&f, funAddr));
		funPtrMap.insert(std::make_pair(funAddr, &f));
	}
}

DynamicValue Interpreter::callFunction(const llvm::Function* f, std::vector<DynamicValue>&& argValues)
{
	assert(f && "f is NULL in runFunction()!");

	if (f->isDeclaration())
		return callExternalFunction(f, std::move(argValues));

	// Make a new stack frame... and fill it in
	auto& calleeFrame = stack.createFrame(f);
	assert(
		(argValues.size() == f->arg_size() ||
		(argValues.size() > f->arg_size() && f->getFunctionType()->isVarArg()))
		&&
		"Invalid number of values passed to function invocation!"
	);

	// Handle non-varargs arguments...
	unsigned i = 0;
	for (auto itr = f->arg_begin(), ite = f->arg_end(); itr != ite; ++itr, ++i)
	{
		calleeFrame.insertBinding(itr, std::move(argValues[i]));
	}

	// Handle varargs arguments...
	for (auto itr = argValues.begin() + i, ite = argValues.end(); itr != ite; ++itr)
		calleeFrame.insertVararg(std::move(*itr));

	return runFunction(calleeFrame);
}

void Interpreter::popStack()
{
	// Cleanup all the allocated memories in this frame
	stackMem.deallocate(stack.getCurrentFrame().getAllocationSize());
	stack.popFrame();
}

DynamicValue Interpreter::runFunction(StackFrame& frame)
{
	// Get the current function
	auto f = frame.getFunction();
	auto curBB = f->begin();

	// This function handles the actual updating of block and instruction iterators as well as execution of all of the PHI nodes in the destination block.
	auto switchToNewBasicBlock = [this, &curBB, &frame] (const BasicBlock* destBB)
	{
		auto prevBB = curBB;
		curBB = destBB;

		// We cannot update the binding for phi nodes on-the-fly because the language semantics require them to be updated "simutaneously". New values need to be cached before they can be committed into the stack frame
		auto phiValueCache = std::vector<std::pair<const PHINode*, DynamicValue>>();
		for (auto const& inst: *curBB)
		{
			auto phiNode = dyn_cast<PHINode>(&inst);
			if (phiNode == nullptr)
				break;

			auto idx = phiNode->getBasicBlockIndex(prevBB);
			assert(idx != -1 && "PHINode doesn't contain entry for predecessor??");
			auto incomingVal = evaluateOperand(frame, phiNode->getIncomingValue(idx));
			phiValueCache.push_back(std::make_pair(phiNode, std::move(incomingVal)));
		}

		for (auto& updatePair: phiValueCache)
		{
			frame.insertBinding(updatePair.first, std::move(updatePair.second));
		}
	};

	while (true)
	{
		// Skip all the phi nodes first
		auto instItr = curBB->begin();
		while (isa<PHINode>(instItr))
			++instItr;

		// Evaluate non-terminator instructions.
		// Those instructions won't alter control flows
		for (auto instIte = curBB->end(); instItr != instIte; ++instItr)
		{
			if (instItr->isTerminator())
				break;

			evaluateInstruction(frame, instItr);
		}

		auto termInst = curBB->getTerminator();
		switch (termInst->getOpcode())
		{
			case Instruction::Br:
			{
				auto brInst = cast<BranchInst>(termInst);

				auto destBB = brInst->getSuccessor(0);
				if (brInst->isConditional())
				{
					auto condVal = evaluateOperand(frame, brInst->getCondition());
					if (!condVal.getAsIntValue().getInt().getBoolValue())
						destBB = brInst->getSuccessor(1);
				}

				switchToNewBasicBlock(destBB);

				break;
			}
			case Instruction::Ret:
			{
				auto retInst = cast<ReturnInst>(termInst);

				auto retVal = DynamicValue::getUndefValue();
				if (auto value = retInst->getReturnValue())
					retVal = evaluateOperand(frame, value);

				// Pop the stack frame
				popStack();

				return retVal;
			}
			case Instruction::Switch:
			{
				auto switchInst = cast<SwitchInst>(termInst);

				auto condVal = evaluateOperand(frame, switchInst->getCondition());
				auto const& condInt = condVal.getAsIntValue().getInt();

				auto const* destBB = switchInst->getDefaultDest();
				for (auto& caseItr: switchInst->cases())
				{
					auto const& caseInt = caseItr.getCaseValue()->getValue();
					if (condInt == caseInt)
					{
						destBB = caseItr.getCaseSuccessor();
						break;
					}
				}

				switchToNewBasicBlock(destBB);

				break;
			}
			case Instruction::Unreachable:
				llvm_unreachable("Reached an unreachable instruction!");
			case Instruction::IndirectBr:
			case Instruction::Invoke:
			case Instruction::Resume:
			default:
				llvm_unreachable("Unsupported terminator instruction");
		}
	}
}

int Interpreter::runMain(const Function* mainFn, const std::vector< std::string> mainArgs)
{
	auto args = std::vector<DynamicValue>();

	// FIXME: Don't ignore main's arguments
	auto retVal = callFunction(mainFn, std::move(args));
	if (retVal.isUndefValue())
		return 0;
	else
		return retVal.getAsIntValue().getInt().getSExtValue();
}
