/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Reordering.h"
#include "log.h"
#include "../intermediate/Helper.h"
#include "../Profiler.h"

using namespace vc4c;
using namespace vc4c::optimizations;
using namespace vc4c::intermediate;

/*
 * Finds the last instruction before the (list of) NOP(s) that is not a NOP -> the reason for the insertion of NOPs
 */
static InstructionWalker findPreviousInstruction(BasicBlock& basicBlock, const InstructionWalker pos)
{
	PROFILE_START(findPreviousInstruction);
	auto it = pos;
	while(!it.isStartOfBlock())
	{
		if(it.get() != nullptr && it->getOutput().hasValue)
			break;
		it.previousInBlock();
	}
	PROFILE_END(findPreviousInstruction);
	return it;
}

/*
 * Finds an instruction within the basic block that does not access any of the given values
 */
static InstructionWalker findInstructionNotAccessing(BasicBlock& basicBlock, const InstructionWalker pos, FastSet<Value>& excludedValues)
{
	std::size_t instrctionsLeft = REPLACE_NOP_MAX_INSTRUCTIONS_TO_CHECK;
	auto it = pos;
	while(instrctionsLeft > 0 && !it.isEndOfBlock())
	{
		if(it.get() == nullptr)
		{
			//skip already replaced instructions
			it.nextInBlock();
			--instrctionsLeft;
			continue;
		}
		bool validReplacement = true;
		PROFILE_START(checkExcludedValues);
		if(it->getOutput().hasValue && excludedValues.find(it->getOutput().get()) != excludedValues.end())
		{
			validReplacement = false;
		}
		for(const Value& arg : it->getArguments())
		{
			if(excludedValues.find(arg) != excludedValues.end())
			{
				validReplacement = false;
				break;
			}
		}
		PROFILE_END(checkExcludedValues);
		//for now, skip everything setting and using flags/signals
		if(validReplacement && (it->hasConditionalExecution() || it->hasSideEffects()))
		{
			validReplacement = false;
		}
		if(validReplacement && (it.has<Branch>() || it.has<BranchLabel>() || it.has<MemoryBarrier>()))
		{
			//TODO prevent from re-ordering over memory fences? Not necessary, unless we re-oder memory accesses
			//NEVER RE-ORDER BRANCHS, LABELS OR BARRIERS!
			validReplacement = false;
		}
		if(validReplacement && it.has<Nop>())
		{
			//replacing NOP with NOP will violate the delay (e.g. for branches, SFU)
			validReplacement = false;
		}
		if(validReplacement && !it->mapsToASMInstruction())
		{
			//skip every instruction, which is not mapped to machine code, since otherwise the delay for the NOP will be violated
			validReplacement = false;
		}
		if(validReplacement && it->getOutput() && it->getOutput().get().hasRegister(REG_MUTEX))
		{
			//never move MUTEX_RELEASE, but MUTEX_ACQUIRE can be moved, so a general test on REG_MUTEX is wrong
			validReplacement = false;
			//Also, never move any instruction over MUTEX_RELEASE to not expand the critical section
			return basicBlock.end();
		}
		if(validReplacement && ((it->getArgument(0) && it->getArgument(0).get().hasRegister(REG_MUTEX)) || (it->getArgument(1) && it->getArgument(1).get().hasRegister(REG_MUTEX))))
		{
			//TODO prevent MUTEX_ACQUIRE from being re-ordered?
			//Re-ordering MUTEX_ACQUIRE extends the critical section (maybe a lot!)
			//-> Would need to prohibit anything after MUTEX_ACQUIRE from being re-ordered (similar to MUTEX_RELEASE)
			//-> only move a maximum amount of instructions (e.g. short more than VPM wait delay)?
			validReplacement = false;
			return basicBlock.end();

		}
		if(validReplacement)
		{
			logging::debug() << "Found instruction not using any of the excluded values (" << to_string<Value, FastSet<Value>>(excludedValues) << "): " << it->to_string() << logging::endl;
			break;
		}

		//otherwise add all outputs by instructions in between (the NOP and the replacement), since they could be used as input in the following instructions
		if(it->getOutput() && !it->getOutput().get().hasRegister(REG_NOP))
		{
			excludedValues.insert(it->getOutput().get());
			//make sure, SFU/TMU calls are not moved over other SFU/TMU calls
			//this prevents nop-sfu-... from being replaced with sfu-sfu-...
			if(it->getOutput().get().hasRegister(REG_SFU_EXP2) || it->getOutput().get().hasRegister(REG_SFU_LOG2) || it->getOutput().get().hasRegister(REG_SFU_RECIP)
					|| it->getOutput().get().hasRegister(REG_SFU_RECIP_SQRT) || it->getOutput().get().hasRegister(REG_TMU_ADDRESS))
			{
				excludedValues.emplace(Value(REG_SFU_EXP2, TYPE_FLOAT));
				excludedValues.emplace(Value(REG_SFU_LOG2, TYPE_FLOAT));
				excludedValues.emplace(Value(REG_SFU_OUT, TYPE_FLOAT));
				excludedValues.emplace(Value(REG_SFU_RECIP, TYPE_FLOAT));
				excludedValues.emplace(Value(REG_SFU_RECIP_SQRT, TYPE_FLOAT));
				excludedValues.emplace(Value(REG_TMU_ADDRESS, TYPE_VOID.toPointerType()));
			}
		}
		--instrctionsLeft;
		it.nextInBlock();
	}
	if(instrctionsLeft == 0)
		it = basicBlock.end();
	return it;
}

/*
 * Finds a suitable instruction within this basic block to replace the NOP with, without violating the reason for the NOP.
 * Also, this instruction MUST not be dependent on any instruction in between the NOP and the replacement-instruction
 */
static InstructionWalker findReplacementCandidate(BasicBlock& basicBlock, const InstructionWalker pos, const DelayType nopReason)
{
	PROFILE_START(findReplacementCandidate);
	FastSet<Value> excludedValues;
	InstructionWalker replacementIt = basicBlock.end();
	switch(nopReason)
	{
		case DelayType::BRANCH_DELAY:
			//This type of NOPs do not yet exist (they are created in CodeGenerator)
			PROFILE_END(findReplacementCandidate);
			return basicBlock.end();
		case DelayType::THREAD_END:
			//there are no more instructions after THREND
			PROFILE_END(findReplacementCandidate);
			return basicBlock.end();
		case DelayType::WAIT_REGISTER:
		{
			//can insert any instruction which does not access the given register/local
			const InstructionWalker lastInstruction = findPreviousInstruction(basicBlock, pos);
			if(lastInstruction.isStartOfBlock())
			{
				//this can e.g. happen, if the vector rotation is the first instruction in a basic block
				//TODO for now, we can't handle this case, since there may be several writing instructions jumping to the block
				logging::debug() << "Can't find reason for NOP in block: " << basicBlock.begin()->to_string() << logging::endl;
				return basicBlock.end();
			}
			excludedValues.insert(lastInstruction->getOutput().get());
			if(lastInstruction->getOutput().get().hasRegister(REG_VPM_IN_ADDR))
			{
				excludedValues.emplace(Value(REG_VPM_IN_BUSY, TYPE_UNKNOWN));
				excludedValues.emplace(Value(REG_VPM_IO, TYPE_UNKNOWN));
			}
			if(lastInstruction->getOutput().get().hasRegister(REG_VPM_OUT_ADDR))
			{
				excludedValues.emplace(Value(REG_VPM_OUT_BUSY, TYPE_UNKNOWN));
				excludedValues.emplace(Value(REG_VPM_IO, TYPE_UNKNOWN));
			}
			PROFILE_START(findInstructionNotAccessing);
			replacementIt = findInstructionNotAccessing(basicBlock, pos, excludedValues);
			PROFILE_END(findInstructionNotAccessing);
			break;
		}
		case DelayType::WAIT_SFU:
		case DelayType::WAIT_TMU:
		{
			//can insert any instruction which doesn't access SFU/TMU or accumulator r4
			excludedValues.emplace(Value(REG_SFU_EXP2, TYPE_FLOAT));
			excludedValues.emplace(Value(REG_SFU_LOG2, TYPE_FLOAT));
			excludedValues.emplace(Value(REG_SFU_OUT, TYPE_FLOAT));
			excludedValues.emplace(Value(REG_SFU_RECIP, TYPE_FLOAT));
			excludedValues.emplace(Value(REG_SFU_RECIP_SQRT, TYPE_FLOAT));
			excludedValues.emplace(Value(REG_TMU_ADDRESS, TYPE_VOID.toPointerType()));
			PROFILE_START(findInstructionNotAccessing);
			replacementIt = findInstructionNotAccessing(basicBlock, pos, excludedValues);
			PROFILE_END(findInstructionNotAccessing);
			break;
		}
	}
	PROFILE_END(findReplacementCandidate);
	return replacementIt;
}

InstructionWalker optimizations::moveInstructionUp(InstructionWalker dest, InstructionWalker it)
{
	/*
	 * a b c d e f
	 * f b c d e a
	 * f a c d e b
	 * f a b d e c
	 * f a b c e d
	 * f a b c d e
	 */
//	InstructionsIterator next = dest;
//	while(next != it)
//	{
//		std::iter_swap(next, it);
//		++next;
//	}

	/*!
	 * a b c d e f
	 * f a b c d e nil
	 * f a b c d e
	 */
	auto res = dest.emplace(it.release());
	it.erase();
	return res;
}

static void replaceNOPs(BasicBlock& basicBlock, Method& method)
{
	InstructionWalker it = basicBlock.begin();
	while(!it.isEndOfBlock())
	{
		const Nop* nop = it.get<Nop>();
		//only replace NOPs without side-effects (e.g. signal)
		if(nop != nullptr && !nop->hasSideEffects())
		{
			InstructionWalker replacementIt = findReplacementCandidate(basicBlock, it, nop->type);
			if(!replacementIt.isEndOfBlock())
			{
				// replace NOP with instruction, reset instruction at position (do not yet erase, otherwise iterators are wrong!)
				logging::debug() << "Replacing NOP with: " << replacementIt->to_string() << logging::endl;
				bool cannotBeCombined = !it->canBeCombined;
				it.reset(replacementIt.release());
				if(cannotBeCombined)
					it->canBeCombined = false;
			}
		}
		it.nextInBlock();
	}
}

void optimizations::splitReadAfterWrites(const Module& module, Method& method, const Configuration& config)
{
	//try to split up consecutive instructions writing/reading to the same local (so less locals are forced to accumulators) by inserting NOPs
	//the NOP then can be replaced with other instructions by the next optimization (#reorderWithinBasicBlocks)
	auto it = method.walkAllInstructions();
	InstructionWalker lastInstruction = it;
	const Local* lastWrittenTo = nullptr;
	//skip the first instruction, since we start the check at the read (and need to look back at the write)
	it.nextInMethod();
	while(!it.isEndOfMethod())
	{
		//skip already replaced instructions
		if(it.get() != nullptr)
		{
			if(lastWrittenTo != nullptr)
			{
				if(it->readsLocal(lastWrittenTo))
				{
					//only insert instruction, if local is used afterwards (and not just in the next few instructions)
					//or the pack-mode is set, since in that case, the register-file A MUST be used, so it cannot be read in the next instruction
					//also vector-rotations MUST be on accumulator, but the input MUST NOT be written in the previous instruction, so they are also split up
					if(lastInstruction->hasPackMode() || it.has<VectorRotation>() || !lastInstruction.getBasicBlock()->isLocallyLimited(lastInstruction, lastWrittenTo))
					{
						logging::debug() << "Inserting NOP to split up read-after-write before: " << it->to_string() << logging::endl;
						//emplacing after the last instruction instead of before this one fixes errors with wrote-label-read, which then becomes
						//write-nop-label-read instead of write-label-nop-read and the combiner can find a reason for the NOP
						lastInstruction.copy().nextInBlock().emplace(new Nop(DelayType::WAIT_REGISTER));
					}
				}
			}
			if(it->mapsToASMInstruction())
			{
				//ignoring instructions not mapped to machine code, e.g. labels will also check for write-label-read
				lastWrittenTo = it->hasValueType(ValueType::LOCAL) ? it->getOutput().get().local : nullptr;
				lastInstruction = it;
			}
		}
		it.nextInMethod();
	}
}

void optimizations::reorderWithinBasicBlocks(const Module& module, Method& method, const Configuration& config)
{
    /*
     * TODO re-order instructions to:
     * 2. combine instructions(try to pair instruction from ADD and MUL ALU together, or moves)
     * 3. split up VPM setup and wait VPM wait, so the delay can be used productively (only possible if we allow reordering over mutex-release).
     *    How many instructions to try to insert? 3?
     */
	for(BasicBlock& block : method.getBasicBlocks())
	{
		// remove NOPs by inserting instructions which do not violate the reason for the NOP
		PROFILE(replaceNOPs, block, method);
	}

	//after all re-orders are done, remove empty instructions
	method.cleanEmptyInstructions();
}

InstructionWalker optimizations::moveRotationSourcesToAccumulators(const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
	//makes sure, all sources for vector-rotations have a usage-range small enough to be on an accumulator
	if(it.has<VectorRotation>() && it.get<VectorRotation>()->getSource().hasType(ValueType::LOCAL))
	{
		const Local* loc = it.get<VectorRotation>()->getSource().local;
		InstructionWalker writer = it.copy().previousInBlock();
		while(!writer.isStartOfBlock())
		{
			if(writer.has() && writer->hasValueType(ValueType::LOCAL) && writer->getOutput().get().hasLocal(loc))
				break;
			writer.previousInBlock();
		}
		//if the local is either written in another block or the usage-range exceeds the accumulator threshold, move to temporary
		if(writer.isStartOfBlock() || !writer.getBasicBlock()->isLocallyLimited(writer, loc))
		{
			InstructionWalker mapper = it.copy().previousInBlock();
			//insert mapper before first NOP
			while(mapper.copy().previousInBlock().has<Nop>())
				mapper.previousInBlock();
			logging::debug() << "Moving source of vector-rotation to temporary for: " << it->to_string() << logging::endl;
			const Value tmp = method.addNewLocal(loc->type, "%vector_rotation");
			mapper.emplace(new MoveOperation(tmp, loc->createReference()));
			it->replaceLocal(loc, tmp.local, LocalUser::Type::READER);
			return writer;
		}
	}
	return it;
}
