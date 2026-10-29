import AppKit
import Foundation

guard CommandLine.arguments.count == 5, ["FileFlow", "DataLux"].contains(CommandLine.arguments[4]) else {
    fputs("usage: render_background.swift OUTPUT.png VERSION ARCH FileFlow|DataLux\n", stderr)
    exit(64)
}

let output = URL(fileURLWithPath: CommandLine.arguments[1])
let version = CommandLine.arguments[2]
let arch = CommandLine.arguments[3]
let product = CommandLine.arguments[4]
let width: CGFloat = 660
let height: CGFloat = 380
let canvas = NSRect(x: 0, y: 0, width: width, height: height)
guard let bitmap = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: Int(width),
    pixelsHigh: Int(height),
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
), let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
    fputs("could not create background bitmap\n", stderr)
    exit(1)
}
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
defer { NSGraphicsContext.restoreGraphicsState() }

let backgroundGradient = NSGradient(colors: [
    NSColor(calibratedRed: 0.035, green: 0.055, blue: 0.063, alpha: 1),
    NSColor(calibratedRed: 0.075, green: 0.105, blue: 0.115, alpha: 1),
])!
backgroundGradient.draw(in: canvas, angle: -72)

NSColor(calibratedWhite: 1, alpha: 0.035).setStroke()
for x in stride(from: CGFloat(20), through: width, by: CGFloat(40)) {
    let line = NSBezierPath()
    line.move(to: NSPoint(x: x, y: 0))
    line.line(to: NSPoint(x: x, y: height))
    line.lineWidth = 0.5
    line.stroke()
}

func glow(center: NSPoint) {
    let rect = NSRect(x: center.x - 92, y: center.y - 92, width: 184, height: 184)
    let gradient = NSGradient(colors: [
        NSColor(calibratedRed: 0.13, green: 0.85, blue: 0.93, alpha: 0.16),
        NSColor(calibratedRed: 0.13, green: 0.85, blue: 0.93, alpha: 0),
    ])!
    gradient.draw(in: NSBezierPath(ovalIn: rect), relativeCenterPosition: .zero)
}

glow(center: NSPoint(x: 170, y: 160))
glow(center: NSPoint(x: 490, y: 160))

func labelBackplate(centerX: CGFloat) {
    // Finder draws its own black icon labels near y=294 from the content area's
    // top edge. This light, quiet surface keeps them readable without duplicating text.
    let rect = NSRect(x: centerX - 80, y: 69, width: 160, height: 30)
    let pill = NSBezierPath(roundedRect: rect, xRadius: 13, yRadius: 13)
    NSColor(calibratedRed: 0.69, green: 0.81, blue: 0.82, alpha: 0.76).setFill()
    pill.fill()
    NSColor(calibratedRed: 0.16, green: 0.78, blue: 0.84, alpha: 0.34).setStroke()
    pill.lineWidth = 1
    pill.stroke()
}

labelBackplate(centerX: 170)
labelBackplate(centerX: 490)

let titleAttributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 31, weight: .semibold),
    .foregroundColor: NSColor(calibratedWhite: 0.96, alpha: 1),
    .kern: -0.7,
]
let title = product as NSString
title.draw(at: NSPoint(x: 36, y: 319), withAttributes: titleAttributes)
let titleWidth = title.size(withAttributes: titleAttributes).width
("." as NSString).draw(
    at: NSPoint(x: 36 + titleWidth - 1, y: 319),
    withAttributes: titleAttributes.merging([.foregroundColor: NSColor(calibratedRed: 0.12, green: 0.86, blue: 0.93, alpha: 1)]) { _, new in new }
)

let subtitleAttributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 15, weight: .medium),
    .foregroundColor: NSColor(calibratedWhite: 0.76, alpha: 1),
    .kern: 0.15,
]
("Drag \(product) to Applications" as NSString).draw(
    at: NSPoint(x: 38, y: 289),
    withAttributes: subtitleAttributes
)

let accent = NSColor(calibratedRed: 0.10, green: 0.82, blue: 0.91, alpha: 0.95)
accent.setStroke()
accent.setFill()
let arrow = NSBezierPath()
arrow.lineWidth = 4
arrow.lineCapStyle = .round
arrow.lineJoinStyle = .round
arrow.move(to: NSPoint(x: 264, y: 160))
arrow.curve(
    to: NSPoint(x: 397, y: 160),
    controlPoint1: NSPoint(x: 302, y: 181),
    controlPoint2: NSPoint(x: 359, y: 181)
)
arrow.stroke()
let arrowHead = NSBezierPath()
arrowHead.move(to: NSPoint(x: 397, y: 160))
arrowHead.line(to: NSPoint(x: 378, y: 174))
arrowHead.line(to: NSPoint(x: 382, y: 153))
arrowHead.close()
arrowHead.fill()

let instructionAttributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 10, weight: .bold),
    .foregroundColor: accent,
    .kern: 1.6,
]
let instruction = "DRAG TO INSTALL" as NSString
let instructionWidth = instruction.size(withAttributes: instructionAttributes).width
instruction.draw(
    at: NSPoint(x: (width - instructionWidth) / 2, y: 125),
    withAttributes: instructionAttributes
)

let footerAttributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.monospacedSystemFont(ofSize: 10, weight: .regular),
    .foregroundColor: NSColor(calibratedWhite: 0.47, alpha: 1),
]
("\(product) \(version)  ·  macOS \(arch)" as NSString).draw(
    at: NSPoint(x: 38, y: 25),
    withAttributes: footerAttributes
)

context.flushGraphics()
guard let png = bitmap.representation(using: .png, properties: [.compressionFactor: 1.0]) else {
    fputs("could not encode background image\n", stderr)
    exit(1)
}
try png.write(to: output, options: .atomic)
