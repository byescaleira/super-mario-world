#if !os(tvOS) && !targetEnvironment(simulator)
import Metal
import MetalKit
import QuartzCore

/// Metal 4 presentation path: explicit command allocators, argument tables,
/// residency sets and barriers, with pipelines built by the MTL4 compiler.
@MainActor
final class Metal4Renderer: FrameRenderer {
    let name = "Metal 4"
    private let device: MTLDevice
    private let queue: MTL4CommandQueue
    private let commandBuffer: MTL4CommandBuffer
    private let allocators: [MTL4CommandAllocator]
    private let renderTable: MTL4ArgumentTable
    private let computeTable: MTL4ArgumentTable
    private var pipelines: [MTLPixelFormat: [String: MTLRenderPipelineState]] = [:]
    private let compiler: MTL4Compiler
    private let library: MTLLibrary
    private let scalePipeline: MTLComputePipelineState
    private let slots: [FrameSlot]
    private let up2x: MTLTexture, up4x: MTLTexture
    private let residency: MTLResidencySet
    private let frameEvent: MTLSharedEvent
    private var frameNumber: UInt64 = 0
    private var layerResidencyAdded = false

    init(device: MTLDevice) throws {
        self.device = device
        guard let queue = device.makeMTL4CommandQueue(),
              let cb = device.makeCommandBuffer(),
              let event = device.makeSharedEvent() else {
            throw ROMError.invalid("Metal 4 queue creation failed")
        }
        self.queue = queue
        commandBuffer = cb
        frameEvent = event
        allocators = try (0..<RendererFactory.inFlight).map { _ in
            guard let a = device.makeCommandAllocator() else { throw ROMError.invalid("allocator") }
            return a
        }

        let rt = MTL4ArgumentTableDescriptor()
        rt.maxBufferBindCount = 1
        rt.maxTextureBindCount = 1
        renderTable = try device.makeArgumentTable(descriptor: rt)
        let ct = MTL4ArgumentTableDescriptor()
        ct.maxTextureBindCount = 2
        computeTable = try device.makeArgumentTable(descriptor: ct)

        let library = try RendererFactory.library(device: device)
        self.library = library
        compiler = try device.makeCompiler(descriptor: MTL4CompilerDescriptor())
        let cd = MTL4ComputePipelineDescriptor()
        cd.computeFunctionDescriptor = Self.function("cs_scale2x", library)
        scalePipeline = try compiler.makeComputePipelineState(descriptor: cd)

        slots = RendererFactory.makeSlots(device: device)
        (up2x, up4x) = RendererFactory.makeUpscaleTextures(device: device)

        let rd = MTLResidencySetDescriptor()
        rd.initialCapacity = slots.count * 3 + 2
        residency = try device.makeResidencySet(descriptor: rd)
        for s in slots {
            residency.addAllocation(s.buffer)
            residency.addAllocation(s.texture)
            residency.addAllocation(s.uniforms)
        }
        residency.addAllocation(up2x)
        residency.addAllocation(up4x)
        residency.commit()
        queue.addResidencySet(residency)
    }

    private static func function(_ name: String, _ library: MTLLibrary) -> MTL4LibraryFunctionDescriptor {
        let f = MTL4LibraryFunctionDescriptor()
        f.name = name
        f.library = library
        return f
    }

    /// Pipelines are built by the MTL4 compiler per drawable pixel format.
    private func pipeline(_ format: MTLPixelFormat, _ name: String) throws -> MTLRenderPipelineState {
        if pipelines[format] == nil {
            var set: [String: MTLRenderPipelineState] = [:]
            for fragment in ["fs_sharp", "fs_crt"] {
                let d = MTL4RenderPipelineDescriptor()
                d.vertexFunctionDescriptor = Self.function("vs_quad", library)
                d.fragmentFunctionDescriptor = Self.function(fragment, library)
                d.colorAttachments[0].pixelFormat = format
                set[fragment] = try compiler.makeRenderPipelineState(descriptor: d)
            }
            pipelines[format] = set
        }
        return pipelines[format]![name]!
    }

    func acquireSlot() -> Int {
        frameNumber += 1
        if frameNumber > UInt64(RendererFactory.inFlight) {
            _ = frameEvent.wait(untilSignaledValue: frameNumber - UInt64(RendererFactory.inFlight), timeoutMS: 100)
        }
        let slot = Int(frameNumber % UInt64(RendererFactory.inFlight))
        allocators[slot].reset()
        return slot
    }

    func pixels(of slot: Int) -> UnsafeMutablePointer<UInt32> { slots[slot].pixels }

    func draw(slot: Int, activeWidth: Int, in view: MTKView, config: VideoConfig) {
        let s = slots[slot]
        if !layerResidencyAdded, let layer = view.layer as? CAMetalLayer {
            queue.addResidencySet(layer.residencySet)
            layerResidencyAdded = true
        }
        guard let drawable = view.currentDrawable,
              let pso = try? pipeline(view.colorPixelFormat, RendererFactory.fragmentName(for: config.filter)) else {
            queue.signalEvent(frameEvent, value: frameNumber)
            return
        }
        var u = RendererFactory.uniforms(drawableSize: view.drawableSize, activeWidth: activeWidth, config: config)
        memcpy(s.uniforms.contents(), &u, MemoryLayout<ShaderUniforms>.stride)

        commandBuffer.beginCommandBuffer(allocator: allocators[slot])
        var source = s.texture
        let smooth = config.filter == .smooth
        if smooth, let ce = commandBuffer.makeComputeCommandEncoder() {
            ce.setComputePipelineState(scalePipeline)
            computeTable.setTexture(s.texture.gpuResourceID, index: 0)
            computeTable.setTexture(up2x.gpuResourceID, index: 1)
            ce.setArgumentTable(computeTable)
            ce.dispatchThreads(threadsPerGrid: MTLSize(width: FrameSlot.textureWidth, height: FrameSlot.height, depth: 1),
                               threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
            ce.barrier(afterEncoderStages: .dispatch, beforeEncoderStages: .dispatch, visibilityOptions: .device)
            computeTable.setTexture(up2x.gpuResourceID, index: 0)
            computeTable.setTexture(up4x.gpuResourceID, index: 1)
            ce.setArgumentTable(computeTable)
            ce.dispatchThreads(threadsPerGrid: MTLSize(width: FrameSlot.textureWidth * 2, height: FrameSlot.height * 2, depth: 1),
                               threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
            ce.endEncoding()
            source = up4x
        }

        let pass = MTL4RenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        if let re = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
            if smooth {
                re.barrier(afterQueueStages: .dispatch, beforeStages: .fragment, visibilityOptions: .device)
            }
            re.setRenderPipelineState(pso)
            renderTable.setAddress(s.uniforms.gpuAddress, index: 0)
            renderTable.setTexture(source.gpuResourceID, index: 0)
            re.setArgumentTable(renderTable, stages: [.vertex, .fragment])
            re.drawPrimitives(primitiveType: .triangleStrip, vertexStart: 0, vertexCount: 4)
            re.endEncoding()
        }
        commandBuffer.endCommandBuffer()

        queue.waitForDrawable(drawable)
        queue.commit([commandBuffer])
        queue.signalDrawable(drawable)
        queue.signalEvent(frameEvent, value: frameNumber)
        drawable.present(afterMinimumDuration: config.minimumFrameDuration)
    }
}
#endif
