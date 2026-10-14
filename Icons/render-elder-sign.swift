#!/usr/bin/env swift
import AppKit
import CoreGraphics
import CoreText
import Foundation

struct Arguments {
  let fontPath: String
  let outputDirectory: String
  let size: Int
  let fillFraction: CGFloat
}

func usage() -> Never {
  fputs("Usage: render-elder-sign.swift --font <arkham.ttf> --output-dir <Icons> [--size 1024] [--fill 0.64]\n", stderr)
  exit(2)
}

func parseArguments(_ raw: [String]) -> Arguments {
  var fontPath: String?
  var outputDirectory: String?
  var size = 1024
  var fillFraction: CGFloat = 0.64
  var index = 1
  while index < raw.count {
    let option = raw[index]
    guard index + 1 < raw.count else { usage() }
    let value = raw[index + 1]
    switch option {
    case "--font":
      fontPath = value
    case "--output-dir":
      outputDirectory = value
    case "--size":
      guard let parsed = Int(value), parsed > 0 else { usage() }
      size = parsed
    case "--fill":
      guard let parsed = Double(value), parsed > 0, parsed <= 1 else { usage() }
      fillFraction = CGFloat(parsed)
    default:
      usage()
    }
    index += 2
  }

  guard let fontPath, let outputDirectory else { usage() }
  return Arguments(fontPath: fontPath, outputDirectory: outputDirectory, size: size, fillFraction: fillFraction)
}

let arguments = parseArguments(CommandLine.arguments)
let fontURL = URL(fileURLWithPath: arguments.fontPath)
let outputURL = URL(fileURLWithPath: arguments.outputDirectory, isDirectory: true)
try FileManager.default.createDirectory(at: outputURL, withIntermediateDirectories: true)

let fontData = try Data(contentsOf: fontURL)
guard let provider = CGDataProvider(data: fontData as CFData),
      let graphicsFont = CGFont(provider) else {
  fputs("Could not load font at \(arguments.fontPath)\n", stderr)
  exit(1)
}
let font = CTFontCreateWithGraphicsFont(graphicsFont, 1000, nil, nil)
var character: UniChar = 0x0058
var glyph = CGGlyph()
guard CTFontGetGlyphsForCharacters(font, &character, &glyph, 1),
      let glyphPath = CTFontCreatePathForGlyph(font, glyph, nil) else {
  fputs("Could not read glyph U+0058 from \(arguments.fontPath)\n", stderr)
  exit(1)
}

let glyphBounds = glyphPath.boundingBoxOfPath
let canvas = CGFloat(arguments.size)
let scale = canvas * arguments.fillFraction / max(glyphBounds.width, glyphBounds.height)
var transform = CGAffineTransform(translationX: canvas / 2, y: canvas / 2)
  .scaledBy(x: scale, y: scale)
  .translatedBy(x: -glyphBounds.midX, y: -glyphBounds.midY)
guard let transformedPath = glyphPath.copy(using: &transform) else {
  fputs("Could not transform glyph path\n", stderr)
  exit(1)
}

let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
guard let context = CGContext(
  data: nil,
  width: arguments.size,
  height: arguments.size,
  bitsPerComponent: 8,
  bytesPerRow: 0,
  space: colorSpace,
  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
) else {
  fputs("Could not create bitmap context\n", stderr)
  exit(1)
}
context.clear(CGRect(x: 0, y: 0, width: canvas, height: canvas))
context.addPath(transformedPath)
context.setFillColor(CGColor(srgbRed: 20.0 / 255.0, green: 18.0 / 255.0, blue: 15.0 / 255.0, alpha: 1))
context.fillPath()
guard let image = context.makeImage() else {
  fputs("Could not render glyph image\n", stderr)
  exit(1)
}
let pngData = NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!
try pngData.write(to: outputURL.appendingPathComponent("AppIcon-foreground.png"))

let locale = Locale(identifier: "en_US_POSIX")
func format(_ value: CGFloat) -> String {
  var text = String(format: "%.3f", locale: locale, Double(value))
  while text.contains(".") && text.last == "0" { text.removeLast() }
  if text.last == "." { text.removeLast() }
  return text == "-0" ? "0" : text
}

func svgPoint(_ point: CGPoint) -> String {
  "\(format(point.x)) \(format(canvas - point.y))"
}

var pathCommands: [String] = []
transformedPath.applyWithBlock { elementPointer in
  let element = elementPointer.pointee
  switch element.type {
  case .moveToPoint:
    pathCommands.append("M \(svgPoint(element.points[0]))")
  case .addLineToPoint:
    pathCommands.append("L \(svgPoint(element.points[0]))")
  case .addQuadCurveToPoint:
    pathCommands.append("Q \(svgPoint(element.points[0])) \(svgPoint(element.points[1]))")
  case .addCurveToPoint:
    pathCommands.append("C \(svgPoint(element.points[0])) \(svgPoint(element.points[1])) \(svgPoint(element.points[2]))")
  case .closeSubpath:
    pathCommands.append("Z")
  @unknown default:
    break
  }
}

let pathData = pathCommands.joined(separator: " ")
try pathData.write(to: outputURL.appendingPathComponent("AppIcon-foreground.path"), atomically: true, encoding: .utf8)
let foregroundSVG = """
<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 \(arguments.size) \(arguments.size)\">
  <path fill=\"#14120F\" d=\"\(pathData)\"/>
</svg>
"""
try foregroundSVG.write(to: outputURL.appendingPathComponent("AppIcon-foreground.svg"), atomically: true, encoding: .utf8)
