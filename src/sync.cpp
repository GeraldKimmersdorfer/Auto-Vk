#include <avk/avk.hpp>

namespace avk
{
	command_pool old_sync::sPoolToAllocCommandBuffersFrom;
	queue* old_sync::sQueueToUse;
	
	void old_sync::presets::default_handler_before_operation(command_buffer_t& aCommandBuffer, pipeline_stage aDestinationStage, std::optional<read_memory_access> aDestinationAccess)
	{
		// We do not know which operation came before. Hence, we have to be overly cautious and
		// establish a (possibly) hefty barrier w.r.t. write access that happened before.
		aCommandBuffer.establish_global_memory_barrier_rw(
			pipeline_stage::all_commands,							aDestinationStage,	// Wait for all previous command before continuing with the operation's command
			write_memory_access{memory_access::any_write_access},	aDestinationAccess	// Make any write access available before making the operation's read access type visible
		);
	}
	
	void old_sync::presets::default_handler_after_operation(command_buffer_t& aCommandBuffer, pipeline_stage aSourceStage, std::optional<write_memory_access> aSourceAccess)
	{
		// We do not know which operation comes after. Hence, we have to be overly cautious and
		// establish a (possibly) hefty barrier w.r.t. read access that happens after.
		aCommandBuffer.establish_global_memory_barrier_rw(
			aSourceStage,	pipeline_stage::all_commands,							// All subsequent stages have to wait until the operation has completed
			aSourceAccess,  read_memory_access{memory_access::any_read_access}		// Make the operation's writes available and visible to all memory stages
		);
	}
	
	avk::unique_function<void(command_buffer_t&, pipeline_stage, std::optional<read_memory_access>)> old_sync::presets::image_copy::wait_for_previous_operations(avk::image_t& aSourceImage, avk::image_t& aDestinationImage)
	{
		return [&aSourceImage, &aDestinationImage](command_buffer_t& aCommandBuffer, pipeline_stage aDestinationStage, std::optional<read_memory_access> aDestinationAccess) {
			// Must transfer the swap chain image's layout:
			aDestinationImage.set_target_layout(vk::ImageLayout::eTransferDstOptimal);
			aCommandBuffer.establish_image_memory_barrier(
				aDestinationImage,
				pipeline_stage::top_of_pipe,					// Wait for nothing
				pipeline_stage::transfer,						// Unblock TRANSFER after the layout transition is done
				std::optional<memory_access>{},					// No pending writes to flush out
				memory_access::transfer_write_access			// Transfer write access must have all required memory visible
			);

			// But, IMPORTANT: must also wait for writing to the image to complete!
			aSourceImage.set_target_layout(vk::ImageLayout::eTransferDstOptimal);
			aCommandBuffer.establish_image_memory_barrier_rw(
				aSourceImage,
				pipeline_stage::all_commands, /* -> */ aDestinationStage,	// Wait for all previous command before continuing with the operation's command
				write_memory_access{memory_access::any_write_access},		// Make any write access available, ...
				aDestinationAccess											// ... before making the operation's read access type visible
			);
		};
	}

	avk::unique_function<void(command_buffer_t&, pipeline_stage, std::optional<write_memory_access>)> old_sync::presets::image_copy::let_subsequent_operations_wait(avk::image_t& aSourceImage, avk::image_t& aDestinationImage)
	{
		return [&aSourceImage, &aDestinationImage, originalLayout = aDestinationImage.current_layout()](command_buffer_t& aCommandBuffer, pipeline_stage aSourceStage, std::optional<write_memory_access> aSourceAccess){
			assert(vk::ImageLayout::eTransferDstOptimal == aDestinationImage.current_layout());
			aDestinationImage.set_target_layout(vk::ImageLayout::eColorAttachmentOptimal); // From transfer-dst into color attachment optimal for further rendering
			aCommandBuffer.establish_image_memory_barrier(
				aDestinationImage,
				pipeline_stage::transfer,							// When the TRANSFER has completed
				pipeline_stage::all_commands,						// Afterwards come further commands
				memory_access::transfer_write_access,				// Copied memory must be available
				memory_access::any_access							// Data must be visible to any read and before any write access
			);
		};
	}

	avk::unique_function<void(command_buffer_t&, pipeline_stage, std::optional<write_memory_access>)> old_sync::presets::image_copy::directly_into_present(avk::image_t& aSourceImage, avk::image_t& aDestinationImage)
	{
		return [&aSourceImage, &aDestinationImage](command_buffer_t& aCommandBuffer, pipeline_stage aSourceStage, std::optional<write_memory_access> aSourceAccess){
			assert(vk::ImageLayout::eTransferDstOptimal == aDestinationImage.current_layout());
			aDestinationImage.set_target_layout(vk::ImageLayout::ePresentSrcKHR); // From transfer-dst into present-src layout
			aCommandBuffer.establish_image_memory_barrier(
				aDestinationImage,
				pipeline_stage::transfer,						// When the TRANSFER has completed
				pipeline_stage::bottom_of_pipe,					// Afterwards comes the semaphore -> present
				memory_access::transfer_write_access,			// Copied memory must be available
				std::optional<memory_access>{}					// Present does not need any memory access specified, it's synced with a semaphore anyways.
			);
			// No further old_sync required
		};
	}
	
	old_sync::old_sync(old_sync&& aOther) noexcept
		: mSpecialSync{ std::move(aOther.mSpecialSync) }
		, mCommandbufferRequest { std::move(aOther.mCommandbufferRequest) }
		, mSemaphoreLifetimeHandler{ std::move(aOther.mSemaphoreLifetimeHandler) }
		, mWaitBeforeSemaphores{ std::move(aOther.mWaitBeforeSemaphores) }
		, mCommandBufferRefOrLifetimeHandler{ std::move(aOther.mCommandBufferRefOrLifetimeHandler) }
		, mCommandBuffer{ std::move(aOther.mCommandBuffer) }
		, mEstablishBarrierBeforeOperationCallback{ std::move(aOther.mEstablishBarrierBeforeOperationCallback) }
		, mEstablishBarrierAfterOperationCallback{ std::move(aOther.mEstablishBarrierAfterOperationCallback) }
		, mQueueToUse{ std::move(aOther.mQueueToUse) }
		, mQueueRecommendation{ std::move(aOther.mQueueRecommendation) }
	{
		aOther.mSpecialSync = sync_type::not_required;
		aOther.mSemaphoreLifetimeHandler = {};
		aOther.mWaitBeforeSemaphores.clear();
		aOther.mCommandBufferRefOrLifetimeHandler = {};
		aOther.mCommandBuffer.reset();
		aOther.mEstablishBarrierBeforeOperationCallback = {};
		aOther.mEstablishBarrierAfterOperationCallback = {};
		aOther.mQueueToUse.reset();
		aOther.mQueueRecommendation.reset();
	}

	old_sync& old_sync::operator=(old_sync&& aOther) noexcept
	{
		mSpecialSync = std::move(aOther.mSpecialSync);
		mCommandbufferRequest = std::move(aOther.mCommandbufferRequest);
		mSemaphoreLifetimeHandler = std::move(aOther.mSemaphoreLifetimeHandler);
		mWaitBeforeSemaphores = std::move(aOther.mWaitBeforeSemaphores);
		mCommandBufferRefOrLifetimeHandler = std::move(aOther.mCommandBufferRefOrLifetimeHandler);
		mCommandBuffer = std::move(aOther.mCommandBuffer);
		mEstablishBarrierBeforeOperationCallback = std::move(aOther.mEstablishBarrierBeforeOperationCallback);
		mEstablishBarrierAfterOperationCallback = std::move(aOther.mEstablishBarrierAfterOperationCallback);
		mQueueToUse = std::move(aOther.mQueueToUse);
		mQueueRecommendation = std::move(aOther.mQueueRecommendation);
		
		aOther.mSpecialSync = sync_type::not_required;
		aOther.mSemaphoreLifetimeHandler = {};
		aOther.mWaitBeforeSemaphores.clear();
		aOther.mCommandBufferRefOrLifetimeHandler = {};
		aOther.mCommandBuffer.reset();
		aOther.mEstablishBarrierBeforeOperationCallback = {};
		aOther.mEstablishBarrierAfterOperationCallback = {};
		aOther.mQueueToUse.reset();
		aOther.mQueueRecommendation.reset();

		return *this;
	}
	
	old_sync::~old_sync()
	{
		if (mCommandBuffer.has_value()) {
			if (get_sync_type() == sync_type::by_return) {
				AVK_LOG_ERROR("Sync is requested 'by_return', but command buffer has not been fetched.");
			}
			else {
				AVK_LOG_ERROR("Command buffer has not been submitted but ak::old_sync instance is destructed. This must be a bug.");
			}
		}
#ifdef _DEBUG
		if (mEstablishBarrierBeforeOperationCallback) {
			AVK_LOG_DEBUG("The before-operation-barrier-callback has never been invoked for this ak::old_sync instance. This can be a bug, but it can be okay as well.");
		}
		if (mEstablishBarrierBeforeOperationCallback) {
			AVK_LOG_DEBUG("The after-operation-barrier-callback has never been invoked for this ak::old_sync instance. This can be a bug, but it can be okay as well.");
		}
#endif
	}
	
	old_sync old_sync::not_required()
	{
		old_sync result;
		result.mSpecialSync = sync_type::not_required; // User explicitely stated that there's no old_sync required.
		return result;
	}
	
	old_sync old_sync::wait_idle(bool aDontWarn)
	{
		old_sync result;
		if (aDontWarn) {
			result.mSpecialSync = sync_type::via_wait_idle_deliberately;
		}
		return result;
	}

	old_sync old_sync::auxiliary_with_barriers(
			old_sync& aMasterSync,
			unique_function<void(command_buffer_t&, pipeline_stage /* destination stage */, std::optional<read_memory_access> /* destination access */)> aEstablishBarrierBeforeOperation,
			unique_function<void(command_buffer_t&, pipeline_stage /* source stage */, std::optional<write_memory_access> /* source access */)> aEstablishBarrierAfterOperation
		)
	{
		// Perform some checks
		const auto stealBeforeHandlerOnDemand = is_about_to_steal_before_handler_on_demand(aEstablishBarrierBeforeOperation);
		const auto stealAfterHandlerOnDemand = is_about_to_steal_after_handler_on_demand(aEstablishBarrierAfterOperation);
		const auto stealBeforeHandlerImmediately = is_about_to_steal_before_handler_immediately(aEstablishBarrierBeforeOperation);
		const auto stealAfterHandlerImmediately = is_about_to_steal_after_handler_immediately(aEstablishBarrierAfterOperation);
		assert(2 != (static_cast<int>(stealBeforeHandlerImmediately) + static_cast<int>(stealBeforeHandlerOnDemand)));
		assert(2 != (static_cast<int>(stealAfterHandlerImmediately) + static_cast<int>(stealAfterHandlerOnDemand)));

		// Possibly steal something
		if (stealBeforeHandlerOnDemand) {
			aEstablishBarrierBeforeOperation = [&aMasterSync](command_buffer_t& cb, pipeline_stage stage, std::optional<read_memory_access> access) {
				// Execute and invalidate:
				auto handler = std::move(aMasterSync.mEstablishBarrierBeforeOperationCallback);
				aMasterSync.mEstablishBarrierBeforeOperationCallback = {};
				if (handler) {
					handler(cb, stage, access);
				}
			};
		}
		else if (stealBeforeHandlerImmediately) {
			aEstablishBarrierBeforeOperation = std::move(aMasterSync.mEstablishBarrierBeforeOperationCallback);
			aMasterSync.mEstablishBarrierBeforeOperationCallback = {};
		}
		
		if (stealAfterHandlerOnDemand) {
			aEstablishBarrierAfterOperation = [&aMasterSync](command_buffer_t& cb, pipeline_stage stage, std::optional<write_memory_access> access) {
				// Execute and invalidate:
				auto handler = std::move(aMasterSync.mEstablishBarrierAfterOperationCallback);
				aMasterSync.mEstablishBarrierAfterOperationCallback = {};
				if (handler) {
					handler(cb, stage, access);
				}
			};
		}
		else if (stealAfterHandlerImmediately) {
			aEstablishBarrierAfterOperation = std::move(aMasterSync.mEstablishBarrierAfterOperationCallback);
			aMasterSync.mEstablishBarrierAfterOperationCallback = {};
		}


		// Prepare a shiny new old_sync instance
		old_sync result;
		result.mCommandBufferRefOrLifetimeHandler = std::ref(aMasterSync.get_or_create_command_buffer()); // <-- Set the command buffer reference, not the lifetime handler
		result.mEstablishBarrierAfterOperationCallback = std::move(aEstablishBarrierAfterOperation);
		result.mEstablishBarrierBeforeOperationCallback = std::move(aEstablishBarrierBeforeOperation);
		// Queues may not be used anyways by auxiliary old_sync instances:
		result.mQueueToUse = {};
		result.mQueueRecommendation = {};
		return result;
	}

	old_sync& old_sync::on_queue(std::reference_wrapper<queue> aQueue)
	{
		mQueueToUse = aQueue;
		return *this;
	}
	
	old_sync::sync_type old_sync::get_sync_type() const
	{
		if (mSemaphoreLifetimeHandler) {
			return sync_type::via_semaphore;
		}
		if (!std::holds_alternative<std::monostate>(mCommandBufferRefOrLifetimeHandler)) {
			return sync_type::via_barrier;
		}
		return mSpecialSync.has_value() ? mSpecialSync.value() : sync_type::via_wait_idle;
	}
	
	std::reference_wrapper<queue> old_sync::queue_to_use() const
	{
//#if defined(_DEBUG) && LOG_LEVEL > 4
//		if (!mQueueToUse.has_value()) {
//			if (mQueueRecommendation.has_value()) {
//				LOG_DEBUG_MEGA_VERBOSE(fmt::format("No queue specified => will submit to queue {} which was recommended by the operation. HTH.", mQueueRecommendation.value().get().queue_index()));
//			}
//			else {
//				LOG_DEBUG_MEGA_VERBOSE("No queue specified => will submit to the graphics queue. HTH.");
//			}
//		}
//#endif
//		//return mQueueToUse.value();
		return *sQueueToUse;
	}

	old_sync& old_sync::create_reusable_commandbuffer()
	{
		mCommandbufferRequest = commandbuffer_request::reusable;
		return *this;
	}
	
	old_sync& old_sync::create_single_use_commandbuffer()
	{
		mCommandbufferRequest = commandbuffer_request::single_use;
		return *this;
	}
	
	command_buffer_t& old_sync::get_or_create_command_buffer()
	{
		if (std::holds_alternative<std::reference_wrapper<command_buffer_t>>(mCommandBufferRefOrLifetimeHandler)) {
			return std::get<std::reference_wrapper<command_buffer_t>>(mCommandBufferRefOrLifetimeHandler).get();
		}

		if (!mCommandBuffer.has_value()) {
			switch (mCommandbufferRequest) {
			case commandbuffer_request::reusable:
				mCommandBuffer = sPoolToAllocCommandBuffersFrom->alloc_command_buffer({});
				break;
			default:
				mCommandBuffer = sPoolToAllocCommandBuffersFrom->alloc_command_buffer(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
				break;
			}
			mCommandBuffer.value()->begin_recording(); // Immediately start recording
		}
		return mCommandBuffer.value().get();
	}
	
	//std::reference_wrapper<queue> old_sync::queue_to_transfer_to() const
	//{
	//	return mQueueToTransferOwnershipTo.value_or(queue_to_use());
	//}
	//
	//bool old_sync::queues_are_the_same() const
	//{
	//	queue& q0 = queue_to_use();
	//	queue& q1 = queue_to_transfer_to();
	//	return q0 == q1;
	//}

	void old_sync::set_queue_hint(std::reference_wrapper<queue> aQueueRecommendation)
	{
		mQueueRecommendation = aQueueRecommendation;
	}
	
	void old_sync::establish_barrier_before_the_operation(pipeline_stage aDestinationPipelineStages, std::optional<read_memory_access> aDestinationMemoryStages)
	{
		if (!mEstablishBarrierBeforeOperationCallback) {
			return; // nothing to do here
		}
		mEstablishBarrierBeforeOperationCallback(get_or_create_command_buffer(), aDestinationPipelineStages, aDestinationMemoryStages);
		mEstablishBarrierBeforeOperationCallback = {};
	}

	void old_sync::establish_barrier_after_the_operation(pipeline_stage aSourcePipelineStages, std::optional<write_memory_access> aSourceMemoryStages)
	{
		if (!mEstablishBarrierAfterOperationCallback) {
			return; // nothing to do here
		}
		mEstablishBarrierAfterOperationCallback(get_or_create_command_buffer(), aSourcePipelineStages, aSourceMemoryStages);
		mEstablishBarrierAfterOperationCallback = {};
	}

	std::optional<command_buffer> old_sync::submit_and_sync()
	{
		queue& queue = queue_to_use();
		auto syncType = get_sync_type();

		// TODO: Use ranges
		std::vector<resource_ownership<semaphore_t>> waitSemaphoreOwnerships;
		for (auto& e : mWaitBeforeSemaphores) {
			waitSemaphoreOwnerships.push_back(owned(e));
		}

		switch (syncType) {
		case sync_type::via_semaphore:
			{
				assert(mSemaphoreLifetimeHandler);
				assert(mCommandBuffer.has_value());
				mCommandBuffer.value()->establish_global_memory_barrier(
					pipeline_stage::all_commands, 
					pipeline_stage::all_commands, 
					std::optional<memory_access>{memory_access::any_access}, 
					std::optional<memory_access>{memory_access::any_access});
				mCommandBuffer.value()->end_recording();	// What started in get_or_create_command_buffer() ends here.
				auto sema = queue.submit_and_handle_with_semaphore(owned(mCommandBuffer.value()), std::move(waitSemaphoreOwnerships));
				mSemaphoreLifetimeHandler(std::move(sema)); // Transfer ownership and be done with it
				mCommandBuffer.reset();						// Command buffer has been moved from. It's gone.
				mWaitBeforeSemaphores.clear();				// Never ever use them again (they have been moved from)
			}
			break;
		case sync_type::via_barrier:
			assert(!std::holds_alternative<std::monostate>(mCommandBufferRefOrLifetimeHandler));
			if (std::holds_alternative<unique_function<void(command_buffer)>>(mCommandBufferRefOrLifetimeHandler)) {
				assert(mCommandBuffer.has_value());
				mCommandBuffer.value()->end_recording();	// What started in get_or_create_command_buffer() ends here.
				queue.submit(referenced(mCommandBuffer.value()), std::optional<resource_reference<semaphore_t>>{});
				std::get<unique_function<void(command_buffer)>>(mCommandBufferRefOrLifetimeHandler)(std::move(mCommandBuffer.value())); // Transfer ownership and be done with it.
				mCommandBuffer.reset();						// Command buffer has been moved from. It's gone.
			}
			else { // Must mean that we are an auxiliary old_sync handler
				assert(std::holds_alternative<std::reference_wrapper<command_buffer_t>>(mCommandBufferRefOrLifetimeHandler));
				// ... that means: Nothing to do here. Master old_sync will submit the command buffer.
			}
			break;
		case sync_type::via_wait_idle:
			//AVK_LOG_WARNING("Performing waitIdle on queue " + std::to_string(queue_to_use().get().queue_index()) + " in order to old_sync because no other type of handler is present.");
		case sync_type::via_wait_idle_deliberately:
			assert(mCommandBuffer.has_value());
			mCommandBuffer.value()->end_recording();		// What started in get_or_create_command_buffer() ends here.
			queue.submit(referenced(mCommandBuffer.value()), std::optional<resource_reference<semaphore_t>>{});
			queue_to_use().get().handle().waitIdle();
			mCommandBuffer.reset();							// Command buffer is fully handled after waitIdle() and can be destroyed.
			break;
		case sync_type::not_required:
			assert(false);
			throw avk::runtime_error("You were wrong with your assumption that there was no old_sync required! => Provide a concrete old_sync strategy!");
		case sync_type::by_return:
		{
			if (!mCommandBuffer.has_value()) {
				throw avk::runtime_error("Something went wrong. There is no command buffer.");
			}
			mCommandBuffer.value()->end_recording();		// What started in get_or_create_command_buffer() ends here.
			auto tmp = std::move(mCommandBuffer.value());
			mCommandBuffer.reset();
			return std::move(tmp);
		}
		case sync_type::by_existing_command_buffer:
			// All good, everything's handled outside.
			return {};
		default:
			assert(false);
			throw avk::logic_error("unknown syncType");
		}

		assert(!mCommandBuffer.has_value());
		return {};
	}
}
