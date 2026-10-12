import Metal
import MetalKit
import SMWCore

/// Layout shared with `Uniforms` in Shaders.msl.
struct ShaderUniforms {
    var scale: SIMD2<Float>
    var sourceSize: SIMD2<Float>
    var outputSize: SIMD2<Float>
    var uvScale: SIMD2<Float>
    var colorMode: Float
    var scanlines: Float
    var mask: Float
    var curvature: Float
    var outputMode: Float
    var hdrPeak: Float
    var wideGamut: Float
    var sdrWhiteNits: Float
}

/// How pixels leave the GPU.
enum DisplayOutput: Equatable {
    case sdr   // bgra8Unorm, sRGB
    case edr   // rgba16Float, extended linear Display P3 (iOS/iPadOS/macOS)
    case pq    // rgba16Float, HDR10 PQ BT.2020 (tvOS → Dolby Vision / HDR10 TVs)

    var pixelFormat: MTLPixelFormat { self == .sdr ? .bgra8Unorm : .rgba16Float }
    var shaderMode: Float {
        switch self {
        case .sdr: 0
        case .edr: 1
        case .pq: 2
        }
    }
}

struct VideoConfig {
    var filter: VideoFilter
    var aspect: AspectMode
    var crtColors: Bool
    var curvature: Bool
    var scanlines: Float
    var output: DisplayOutput
    var hdrPeak: Float
    var wideGamut: Bool
}

/// One frame's worth of GPU-visible memory. The core renders straight into
/// `pixels`, which aliases `texture` (unified memory, no upload copy).
struct FrameSlot {
    static let textureWidth = Int(SMW_MAX_FRAME_WIDTH)
    static let height = Int(SMW_SCREEN_HEIGHT)
    let buffer: MTLBuffer
    let texture: MTLTexture
    let uniforms: MTLBuffer
    /// The simulator can't alias textures onto shared buffers; there we blit.
    let needsBlit: Bool
    var pixels: UnsafeMutablePointer<UInt32> {
        buffer.contents().bindMemory(to: UInt32.self, capacity: Self.textureWidth * Self.height)
    }
}

@MainActor
protocol FrameRenderer: AnyObject {
    var name: String { get }
    /// Blocks until a frame slot is free and returns it for the core to fill.
    func acquireSlot() -> Int
    func pixels(of slot: Int) -> UnsafeMutablePointer<UInt32>
    /// `activeWidth` is how many of the 512 texture columns hold picture.
    func draw(slot: Int, activeWidth: Int, in view: MTKView, config: VideoConfig)
}

enum RendererFactory {
    static let inFlight = 3

    @MainActor
    static func make(device: MTLDevice) throws -> FrameRenderer {
        #if !os(tvOS) && !targetEnvironment(simulator)
        if device.supportsFamily(.metal4) {
            do { return try Metal4Renderer(device: device) } catch {
                print("Metal 4 unavailable (\(error)); falling back")
            }
        }
        #endif
        return try ClassicRenderer(device: device)
    }

    /// Shaders ship as source and are compiled once at launch (~50 ms), so the
    /// project builds without the offline Metal toolchain.
    static func library(device: MTLDevice) throws -> MTLLibrary {
        guard let url = Bundle.module.url(forResource: "Shaders", withExtension: "msl") else {
            throw ROMError.invalid("Shaders.msl missing from bundle")
        }
        let source = try String(contentsOf: url, encoding: .utf8)
        let options = MTLCompileOptions()
        options.mathMode = .fast
        return try device.makeLibrary(source: source, options: options)
    }

    static func makeSlots(device: MTLDevice) -> [FrameSlot] {
        let width = FrameSlot.textureWidth, height = FrameSlot.height
        let bytesPerRow = width * 4
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width,
                                                            height: height, mipmapped: false)
        desc.storageMode = .shared
        desc.usage = [.shaderRead]
        return (0..<inFlight).map { i in
            let buffer = device.makeBuffer(length: bytesPerRow * height, options: [.storageModeShared])!
            buffer.label = "Frame \(i)"
            memset(buffer.contents(), 0, buffer.length)
            #if targetEnvironment(simulator)
            desc.storageMode = .private
            let texture = device.makeTexture(descriptor: desc)!
            let needsBlit = true
            #else
            let texture = buffer.makeTexture(descriptor: desc, offset: 0, bytesPerRow: bytesPerRow)!
            let needsBlit = false
            #endif
            let uniforms = device.makeBuffer(length: MemoryLayout<ShaderUniforms>.stride, options: [.storageModeShared])!
            return FrameSlot(buffer: buffer, texture: texture, uniforms: uniforms, needsBlit: needsBlit)
        }
    }

    static func makeUpscaleTextures(device: MTLDevice) -> (MTLTexture, MTLTexture) {
        func tex(_ s: Int) -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: FrameSlot.textureWidth * s,
                                                             height: FrameSlot.height * s, mipmapped: false)
            d.storageMode = .private
            d.usage = [.shaderRead, .shaderWrite]
            return device.makeTexture(descriptor: d)!
        }
        return (tex(2), tex(4))
    }

    static func fragmentName(for filter: VideoFilter) -> String {
        filter == .crt ? "fs_crt" : "fs_sharp"
    }

    static func makePipelines(device: MTLDevice, library: MTLLibrary,
                              format: MTLPixelFormat) throws -> [String: MTLRenderPipelineState] {
        var out: [String: MTLRenderPipelineState] = [:]
        for fragment in ["fs_sharp", "fs_crt"] {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "vs_quad")
            d.fragmentFunction = library.makeFunction(name: fragment)
            d.colorAttachments[0].pixelFormat = format
            out[fragment] = try device.makeRenderPipelineState(descriptor: d)
        }
        return out
    }

    /// Display aspect of the frame for a given width in SNES pixels.
    static func displayAspect(width: Int, aspect: AspectMode) -> Double? {
        aspect.pixelAspect.map { Double(width) * $0 / Double(FrameSlot.height) }
    }

    /// Fits the game image into the drawable and fills the shader uniforms.
    static func uniforms(drawableSize: CGSize, activeWidth: Int, config: VideoConfig) -> ShaderUniforms {
        let dw = max(drawableSize.width, 1), dh = max(drawableSize.height, 1)
        var w = dw, h = dh
        if let ratio = displayAspect(width: activeWidth, aspect: config.aspect) {
            if dw / dh > ratio { w = dh * ratio } else { h = dw / ratio }
        }
        let factor: Float = config.filter == .smooth ? 4 : 1
        let source = SIMD2<Float>(Float(activeWidth) * factor, Float(FrameSlot.height) * factor)
        return ShaderUniforms(
            scale: [Float(w / dw), Float(h / dh)],
            sourceSize: source,
            outputSize: [Float(w), Float(h)],
            uvScale: [Float(activeWidth) / Float(FrameSlot.textureWidth), 1],
            colorMode: config.crtColors ? 1 : 0,
            scanlines: config.scanlines,
            mask: config.filter == .crt ? 0.35 : 0,
            curvature: config.filter == .crt && config.curvature ? 1 : 0,
            outputMode: config.output.shaderMode,
            hdrPeak: config.hdrPeak,
            wideGamut: config.wideGamut ? 1 : 0,
            sdrWhiteNits: 203
        )
    }
}

// MARK: - Classic Metal (tvOS, simulator and older GPUs)

@MainActor
final class ClassicRenderer: FrameRenderer {
    let name = "Metal"
    private let device: MTLDevice
    private let library: MTLLibrary
    private let queue: MTLCommandQueue
    private var pipelines: [MTLPixelFormat: [String: MTLRenderPipelineState]] = [:]
    private let scalePipeline: MTLComputePipelineState
    private let slots: [FrameSlot]
    private let up2x: MTLTexture, up4x: MTLTexture
    private let semaphore = DispatchSemaphore(value: RendererFactory.inFlight)
    private var next = 0

    init(device: MTLDevice) throws {
        self.device = device
        queue = device.makeCommandQueue()!
        library = try RendererFactory.library(device: device)
        scalePipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "cs_scale2x")!)
        slots = RendererFactory.makeSlots(device: device)
        (up2x, up4x) = RendererFactory.makeUpscaleTextures(device: device)
    }

    private func pipeline(_ format: MTLPixelFormat, _ name: String) throws -> MTLRenderPipelineState {
        if pipelines[format] == nil {
            pipelines[format] = try RendererFactory.makePipelines(device: device, library: library, format: format)
        }
        return pipelines[format]![name]!
    }

    func acquireSlot() -> Int {
        semaphore.wait()
        let slot = next
        next = (next + 1) % slots.count
        return slot
    }

    func pixels(of slot: Int) -> UnsafeMutablePointer<UInt32> { slots[slot].pixels }

    func draw(slot: Int, activeWidth: Int, in view: MTKView, config: VideoConfig) {
        let s = slots[slot]
        guard let drawable = view.currentDrawable, let pass = view.currentRenderPassDescriptor,
              let pso = try? pipeline(view.colorPixelFormat, RendererFactory.fragmentName(for: config.filter)),
              let cb = queue.makeCommandBuffer() else {
            semaphore.signal()
            return
        }
        var u = RendererFactory.uniforms(drawableSize: view.drawableSize, activeWidth: activeWidth, config: config)
        memcpy(s.uniforms.contents(), &u, MemoryLayout<ShaderUniforms>.stride)

        if s.needsBlit, let blit = cb.makeBlitCommandEncoder() {
            blit.copy(from: s.buffer, sourceOffset: 0, sourceBytesPerRow: FrameSlot.textureWidth * 4,
                      sourceBytesPerImage: FrameSlot.textureWidth * FrameSlot.height * 4,
                      sourceSize: MTLSize(width: FrameSlot.textureWidth, height: FrameSlot.height, depth: 1),
                      to: s.texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
        var source = s.texture
        if config.filter == .smooth, let ce = cb.makeComputeCommandEncoder() {
            ce.setComputePipelineState(scalePipeline)
            ce.setTexture(s.texture, index: 0)
            ce.setTexture(up2x, index: 1)
            ce.dispatchThreads(MTLSize(width: FrameSlot.textureWidth, height: FrameSlot.height, depth: 1),
                               threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
            ce.setTexture(up2x, index: 0)
            ce.setTexture(up4x, index: 1)
            ce.dispatchThreads(MTLSize(width: FrameSlot.textureWidth * 2, height: FrameSlot.height * 2, depth: 1),
                               threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
            ce.endEncoding()
            source = up4x
        }

        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        if let re = cb.makeRenderCommandEncoder(descriptor: pass) {
            re.setRenderPipelineState(pso)
            re.setVertexBuffer(s.uniforms, offset: 0, index: 0)
            re.setFragmentBuffer(s.uniforms, offset: 0, index: 0)
            re.setFragmentTexture(source, index: 0)
            re.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            re.endEncoding()
        }
        let semaphore = self.semaphore
        cb.addCompletedHandler { _ in semaphore.signal() }
        cb.present(drawable)
        cb.commit()
    }
}
