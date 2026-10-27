import Foundation
import Metal

// Compile the production shader and verify the guest BGRA layout, orientation,
// and opaque output without opening a window or starting a guest.
@main struct NativeMetalTest {
    static func main() throws {
        let source = try String(contentsOfFile: "platform/macos/PCEmu/GuestDisplay.swift", encoding: .utf8)
        let shader = source.components(separatedBy: "\"\"\"")[1]
        guard let device = MTLCreateSystemDefaultDevice() else { fatalError("Metal unavailable") }
        let library = try device.makeLibrary(source: shader, options: nil)
        let pipeline = MTLRenderPipelineDescriptor()
        pipeline.vertexFunction = library.makeFunction(name: "screenVertex")
        pipeline.fragmentFunction = library.makeFunction(name: "screenFragment")
        pipeline.colorAttachments[0].pixelFormat = .bgra8Unorm
        let state = try device.makeRenderPipelineState(descriptor: pipeline)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
            width: 2, height: 2, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead, .renderTarget]
        let input = device.makeTexture(descriptor: descriptor)!
        let output = device.makeTexture(descriptor: descriptor)!
        let pixels: [UInt32] = [0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff]
        pixels.withUnsafeBytes {
            input.replace(region: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0,
                withBytes: $0.baseAddress!, bytesPerRow: 8)
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = output
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        let command = device.makeCommandQueue()!.makeCommandBuffer()!
        let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        encoder.setFragmentTexture(input, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        precondition(command.status == .completed, "Metal command failed: \(String(describing: command.error))")
        var result = [UInt32](repeating: 0, count: 4)
        result.withUnsafeMutableBytes {
            output.getBytes($0.baseAddress!, bytesPerRow: 8, from: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0)
        }
        precondition(result == pixels.map { $0 | 0xff000000 }, "Incorrect Metal color, alpha or orientation: \(result)")
        print("Headless Metal shader: BGRA colors, orientation and opaque output passed")
    }
}
