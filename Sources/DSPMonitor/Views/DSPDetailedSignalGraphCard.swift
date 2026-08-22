// DSPDetailedSignalGraphCard.swift - Official CamillaDSP Pipeline Graph Engine faithful port of pycamilladsp-plot by Henrik Enquist

import AppKit
import DSPConfig
import Observation
import SwiftUI

struct DSPDetailedSignalGraphCard: View {
  @Environment(AudioDeviceManager.self) var devices
  @Environment(PipelineStore.self) var pipeline

  private var captureChannels: Int { max(1, devices.captureConfig.channels) }
  private var playbackChannels: Int { max(1, devices.playbackConfig.channels) }
  private var sampleRate: Int { devices.captureConfig.sampleRate }

  // MARK: - Interactive Drag & Custom Positions State

  @State private var customPositions: [String: CGPoint] = [:]
  @State private var activeDragOffset: (id: String, translation: CGSize)? = nil

  // MARK: - Scaling & Dimension Constants

  private let xStep: CGFloat = 130 // Horizontal step distance between columns
  private let yStep: CGFloat = 46  // Vertical distance between channel rows
  private let blockWidth: CGFloat = 85
  private let blockHeight: CGFloat = 28
  private let canvasPadding: CGFloat = 30
  private let titleHeaderHeight: CGFloat = 36

  // MARK: - Engine Graph Elements

  private struct GraphBlock: Identifiable {
    let id: String
    let label: String
    var x: CGFloat
    let y: CGFloat
    let width: CGFloat
    let height: CGFloat
    let isChannelPort: Bool
    var stepIndex: Int
  }

  private struct ContainerBox: Identifiable {
    let id: String
    let label: String
    var centerX: CGFloat
    let activeChannelsCount: Int
    let width: CGFloat
    let height: CGFloat
    let centerY: CGFloat
    let containedBlockIds: [String]
    var stepIndex: Int
  }

  private struct GraphArrow: Identifiable {
    let id: String
    let fromBlockId: String
    let toBlockId: String
    var fromFallback: CGPoint
    var toFallback: CGPoint
    let label: String?
  }

  private static func calculateBlockSize(label: String, isChannelPort: Bool) -> CGSize {
    let fontSize: CGFloat = isChannelPort ? 11 : 10
    let weight: NSFont.Weight = isChannelPort ? .bold : .semibold
    let font = NSFont.monospacedSystemFont(ofSize: fontSize, weight: weight)
    let attrString = NSAttributedString(string: label, attributes: [.font: font])
    let textSize = attrString.size()
    let textWidth = textSize.width
    let textHeight = textSize.height

    let minWidth: CGFloat = isChannelPort ? 48.0 : 85.0
    let paddingH: CGFloat = isChannelPort ? 16.0 : 24.0
    let width = max(minWidth, textWidth + paddingH)
    let height = max(28.0, textHeight + 8.0)
    return CGSize(width: width, height: height)
  }

  // MARK: - Execute Official pycamilladsp-plot Pipeline Placement Engine

  private var graphData: (
    blocks: [GraphBlock],
    boxes: [ContainerBox],
    arrows: [GraphArrow],
    bounds: CGRect
  ) {
    var blocks: [GraphBlock] = []
    var boxes: [ContainerBox] = []
    var arrows: [GraphArrow] = []

    var stages: [[[GraphBlock]]] = []
    var totalLength: Int = 0
    var stageStart: Int = 0
    var activeChannels: Int = captureChannels

    var minY: CGFloat = 0
    var maxY: CGFloat = 0

    func registerY(_ y: CGFloat) {
      minY = min(minY, y)
      maxY = max(maxY, y)
    }

    // Vertically centers stage channel blocks symmetrically around Y = 0
    func yPos(channel: Int, activeChannelsInStage: Int, isPassthrough: Bool = false) -> CGFloat {
      if isPassthrough {
        let passthroughIdx = channel - 4
        let y = (2.0 + CGFloat(passthroughIdx)) * yStep
        registerY(y)
        return y
      }

      let y = (-CGFloat(activeChannelsInStage) / 2.0 + 0.5 + CGFloat(channel)) * yStep
      registerY(y)
      return y
    }

    func makeContainerBox(id: String, label: String, stepIndex: Int, blocksInBox: [GraphBlock]) -> ContainerBox {
      var minBlockY: CGFloat = 0
      var maxBlockY: CGFloat = 0
      var maxBlockW: CGFloat = 48.0
      if !blocksInBox.isEmpty {
        minBlockY = blocksInBox[0].y
        maxBlockY = blocksInBox[0].y
        maxBlockW = blocksInBox[0].width
        for b in blocksInBox {
          minBlockY = min(minBlockY, b.y)
          maxBlockY = max(maxBlockY, b.y)
          maxBlockW = max(maxBlockW, b.width)
        }
      }
      let centerY = (minBlockY + maxBlockY) / 2.0
      let height = (maxBlockY - minBlockY) + blockHeight + 20

      let headerFont = NSFont.monospacedSystemFont(ofSize: 11, weight: .bold)
      let titleWidth = NSAttributedString(string: label, attributes: [.font: headerFont]).size().width
      let width = max(76.0, max(maxBlockW + 28.0, titleWidth + 24.0))

      return ContainerBox(
        id: id,
        label: label,
        centerX: 0,
        activeChannelsCount: blocksInBox.count,
        width: width,
        height: height,
        centerY: centerY,
        containedBlockIds: blocksInBox.map(\.id),
        stepIndex: stepIndex
      )
    }

    // 1. INPUT STAGE
    var captureStageChannels: [[GraphBlock]] = []
    var captureInputBlocks: [GraphBlock] = []
    for n in 0..<activeChannels {
      let y = yPos(channel: n, activeChannelsInStage: activeChannels, isPassthrough: false)
      let label = "\(n + 1)"
      let sz = Self.calculateBlockSize(label: label, isChannelPort: true)
      let b = GraphBlock(
        id: "input_ch\(n)",
        label: label,
        x: 0,
        y: y,
        width: sz.width,
        height: sz.height,
        isChannelPort: true,
        stepIndex: 0
      )
      blocks.append(b)
      captureInputBlocks.append(b)
      captureStageChannels.append([b])
    }

    let captureName = devices.captureConfig.deviceName ?? "Capture Input"
    boxes.append(
      makeContainerBox(
        id: "box_input",
        label: captureName,
        stepIndex: 0,
        blocksInBox: captureInputBlocks
      )
    )
    stages.append(captureStageChannels)

    // 2. PIPELINE STEPS LOOP
    for stage in pipeline.stages {
      guard stage.isEnabled && stage.isActive else { continue }

      let currentStageInputChannels = stages.last?.count ?? activeChannels

      let steps = stage.buildPipelineSteps(
        channelCount: currentStageInputChannels,
        eqPresets: pipeline.eqPresets,
        convPresets: pipeline.convPresets,
        sampleRate: sampleRate
      )

      let stageMixers = stage.buildMixers(
        channels: currentStageInputChannels
      )

      var stageFilterBlockCounts: [Int: Int] = [:]

      for step in steps {
        if step.type == .mixer {
          totalLength += 1
          let name = step.name ?? stage.name

          let mixconf = stageMixers[step.name ?? ""] ?? stageMixers.values.first
          let rawOutCount = mixconf?.channelsOut ?? stage.mixerChannelsOut

          let is2to4 = step.name?.contains("2to4") == true
          let is4to2 = step.name?.contains("4to2") == true

          // Crossfeed expansion & contraction block sizes (2to4 = 4 ports, 4to2 = 2 ports)
          let outChannels: Int = {
            if is2to4 { return 4 }
            if is4to2 { return 2 }
            return rawOutCount
          }()

          activeChannels = outChannels

          var mixerStageChannels: [[GraphBlock]] = []
          var mixerBoxBlocks: [GraphBlock] = []
          for n in 0..<outChannels {
            let y = yPos(channel: n, activeChannelsInStage: outChannels, isPassthrough: false)
            let label = "\(n + 1)"
            let sz = Self.calculateBlockSize(label: label, isChannelPort: true)
            let b = GraphBlock(
              id: "mixer_\(totalLength)_ch\(n)",
              label: label,
              x: 0,
              y: y,
              width: sz.width,
              height: sz.height,
              isChannelPort: true,
              stepIndex: totalLength
            )
            blocks.append(b)
            mixerBoxBlocks.append(b)
            mixerStageChannels.append([b])
          }

          var mappedSourcesInBox = Set<Int>()

          // Draw mixer arrows according to matrix mapping for valid destination channels
          if let mappingList = mixconf?.mapping, !mappingList.isEmpty {
            for mapping in mappingList {
              let destCh = mapping.dest
              guard destCh < outChannels else { continue }

              for src in mapping.sources {
                let srcCh = src.channel
                guard let prevStage = stages.last, srcCh < prevStage.count, let srcBlock = prevStage[srcCh].last else { continue }

                mappedSourcesInBox.insert(srcCh)

                let destBlock = mixerStageChannels[destCh][0]
                let g = src.gain ?? 0.0
                var labelStr = (g == 0.0) ? "0 dB" : String(format: "%+.1f dB", g)
                if src.inverted == true {
                  labelStr += "\ninv."
                }

                arrows.append(
                  GraphArrow(
                    id: "arrow_mix_\(totalLength)_\(srcCh)_\(destCh)",
                    fromBlockId: srcBlock.id,
                    toBlockId: destBlock.id,
                    fromFallback: CGPoint(x: 0, y: srcBlock.y),
                    toFallback: CGPoint(x: 0, y: destBlock.y),
                    label: labelStr
                  )
                )
              }
            }
          } else {
            // Fallback 1-to-1 arrows
            for n in 0..<outChannels {
              let srcCh = min(n, stages.last?.count ?? 1 - 1)
              if let srcBlock = stages.last?[srcCh].last {
                mappedSourcesInBox.insert(srcCh)
                let destBlock = mixerStageChannels[n][0]
                arrows.append(
                  GraphArrow(
                    id: "arrow_mix_fb_\(totalLength)_\(n)",
                    fromBlockId: srcBlock.id,
                    toBlockId: destBlock.id,
                    fromFallback: CGPoint(x: 0, y: srcBlock.y),
                    toFallback: CGPoint(x: 0, y: destBlock.y),
                    label: "0 dB"
                  )
                )
              }
            }
          }

          // Prepare next stage channel state: preserve unconsumed passthrough channels directly without intermediate stops
          var nextStage: [[GraphBlock]] = mixerStageChannels
          if let prevStage = stages.last {
            for c in 0..<prevStage.count {
              if !mappedSourcesInBox.contains(c) {
                nextStage.append(prevStage[c])
              }
            }
          }

          stages.append(nextStage)

          boxes.append(
            makeContainerBox(
              id: "box_mixer_\(totalLength)",
              label: readableMixerTitle(name, inCh: mixconf?.channelsIn ?? currentStageInputChannels, outCh: outChannels),
              stepIndex: totalLength,
              blocksInBox: mixerBoxBlocks
            )
          )

          stageStart = totalLength

        } else if step.type == .filter {
          let chNbrs = step.channels ?? Array(0..<activeChannels)
          let namesToUnroll = step.names ?? (step.name != nil ? [step.name!] : [stage.name])

          for chNbr in chNbrs {
            guard let prevStage = stages.last, chNbr < prevStage.count else { continue }

            for rawName in namesToUnroll {
              let name = readableFilterStepName(rawName, stage: stage)
              let countInStage = stageFilterBlockCounts[chNbr, default: 0]
              let chStep = stageStart + 1 + countInStage
              totalLength = max(totalLength, chStep)
              stageFilterBlockCounts[chNbr] = countInStage + 1

              let y = yPos(channel: chNbr, activeChannelsInStage: activeChannels, isPassthrough: false)
              let sz = Self.calculateBlockSize(label: name, isChannelPort: false)

              let b = GraphBlock(
                id: "filter_\(chStep)_\(chNbr)_\(rawName)",
                label: name,
                x: 0,
                y: y,
                width: sz.width,
                height: sz.height,
                isChannelPort: false,
                stepIndex: chStep
              )

              blocks.append(b)

              if let srcBlock = stages.last?[chNbr].last {
                arrows.append(
                  GraphArrow(
                    id: "arrow_filter_\(chStep)_\(chNbr)_\(rawName)",
                    fromBlockId: srcBlock.id,
                    toBlockId: b.id,
                    fromFallback: CGPoint(x: 0, y: srcBlock.y),
                    toFallback: CGPoint(x: 0, y: b.y),
                    label: nil
                  )
                )
              }

              stages[stages.count - 1][chNbr].append(b)
            }
          }

        } else if step.type == .processor {
          totalLength += 1
          let name = step.name ?? stage.name

          var procStageChannels: [[GraphBlock]] = []
          var procBoxBlocks: [GraphBlock] = []
          for n in 0..<activeChannels {
            let y = yPos(channel: n, activeChannelsInStage: activeChannels, isPassthrough: false)
            let label = "\(n + 1)"
            let sz = Self.calculateBlockSize(label: label, isChannelPort: true)
            let b = GraphBlock(
              id: "proc_\(totalLength)_ch\(n)",
              label: label,
              x: 0,
              y: y,
              width: sz.width,
              height: sz.height,
              isChannelPort: true,
              stepIndex: totalLength
            )
            blocks.append(b)
            procBoxBlocks.append(b)
            procStageChannels.append([b])

            if let srcBlock = stages.last?[n].last {
              arrows.append(
                GraphArrow(
                  id: "arrow_proc_\(totalLength)_\(n)",
                  fromBlockId: srcBlock.id,
                  toBlockId: b.id,
                  fromFallback: CGPoint(x: 0, y: srcBlock.y),
                  toFallback: CGPoint(x: 0, y: b.y),
                  label: nil
                )
              )
            }
          }

          var nextStage: [[GraphBlock]] = procStageChannels
          if let prevStage = stages.last {
            for c in activeChannels..<prevStage.count {
              nextStage.append(prevStage[c])
            }
          }

          stages.append(nextStage)

          boxes.append(
            makeContainerBox(
              id: "box_proc_\(totalLength)",
              label: name,
              stepIndex: totalLength,
              blocksInBox: procBoxBlocks
            )
          )

          stageStart = totalLength
        }
      }

      stageStart = totalLength
    }

    // 3. PLAYBACK OUTPUT STAGE
    totalLength += 1
    var playStageChannels: [[GraphBlock]] = []
    var playBoxBlocks: [GraphBlock] = []
    for n in 0..<activeChannels {
      let y = yPos(channel: n, activeChannelsInStage: activeChannels, isPassthrough: false)
      let label = "\(n + 1)"
      let sz = Self.calculateBlockSize(label: label, isChannelPort: true)
      let b = GraphBlock(
        id: "output_ch\(n)",
        label: label,
        x: 0,
        y: y,
        width: sz.width,
        height: sz.height,
        isChannelPort: true,
        stepIndex: totalLength
      )
      blocks.append(b)
      playBoxBlocks.append(b)
      playStageChannels.append([b])

      if let srcBlock = stages.last?[n].last {
        arrows.append(
          GraphArrow(
            id: "arrow_play_\(n)",
            fromBlockId: srcBlock.id,
            toBlockId: b.id,
            fromFallback: CGPoint(x: 0, y: srcBlock.y),
            toFallback: CGPoint(x: 0, y: b.y),
            label: nil
          )
        )
      }
    }

    let playName = devices.playbackConfig.deviceName ?? "Playback Output"
    boxes.append(
      makeContainerBox(
        id: "box_output",
        label: playName,
        stepIndex: totalLength,
        blocksInBox: playBoxBlocks
      )
    )

    // Layout resolution: calculate column widths and X positions
    var columnWidths = Array(repeating: CGFloat(0), count: totalLength + 1)
    for b in blocks {
      if b.stepIndex >= 0 && b.stepIndex <= totalLength {
        columnWidths[b.stepIndex] = max(columnWidths[b.stepIndex], b.width)
      }
    }
    for box in boxes {
      if box.stepIndex >= 0 && box.stepIndex <= totalLength {
        columnWidths[box.stepIndex] = max(columnWidths[box.stepIndex], box.width)
      }
    }

    var xPositions = Array(repeating: CGFloat(0), count: totalLength + 1)
    xPositions[0] = 0.0
    if totalLength >= 1 {
      for s in 1...totalLength {
        let prevHalf = columnWidths[s - 1] / 2.0
        let currHalf = columnWidths[s] / 2.0
        let minSpacing = xStep
        let neededSpacing = prevHalf + 48.0 + currHalf
        xPositions[s] = xPositions[s - 1] + max(minSpacing, neededSpacing)
      }
    }

    for i in 0..<blocks.count {
      let step = blocks[i].stepIndex
      if step >= 0 && step <= totalLength {
        blocks[i].x = xPositions[step]
      }
    }

    for i in 0..<boxes.count {
      let step = boxes[i].stepIndex
      if step >= 0 && step <= totalLength {
        boxes[i].centerX = xPositions[step]
      }
    }

    let blocksMap = Dictionary(uniqueKeysWithValues: blocks.map { ($0.id, $0) })

    for i in 0..<arrows.count {
      if let srcBlock = blocksMap[arrows[i].fromBlockId] {
        arrows[i].fromFallback = CGPoint(x: srcBlock.x + srcBlock.width / 2.0, y: srcBlock.y)
      }
      if let destBlock = blocksMap[arrows[i].toBlockId] {
        arrows[i].toFallback = CGPoint(x: destBlock.x - destBlock.width / 2.0, y: destBlock.y)
      }
    }

    let lastX = (totalLength >= 0 && totalLength < xPositions.count) ? xPositions[totalLength] : 0.0
    let lastColW = (totalLength >= 0 && totalLength < columnWidths.count) ? columnWidths[totalLength] : 76.0
    let totalWidth = lastX + lastColW / 2.0 + canvasPadding * 2 + 40
    let totalHeight = (maxY - minY) + canvasPadding * 2 + titleHeaderHeight + 40
    let bounds = CGRect(x: 0, y: 0, width: totalWidth, height: totalHeight)

    return (blocks, boxes, arrows, bounds)
  }

  // MARK: - Dynamic Real-Time Position Resolvers

  private func currentBlockPosition(b: GraphBlock, originY: CGFloat) -> CGPoint {
    let basePos = customPositions[b.id] ?? CGPoint(x: b.x + canvasPadding + 40, y: originY + b.y)
    if let active = activeDragOffset, active.id == b.id {
      return CGPoint(x: basePos.x + active.translation.width, y: basePos.y + active.translation.height)
    }
    return basePos
  }

  private func boxFrame(box: ContainerBox, blocksMap: [String: GraphBlock], originY: CGFloat) -> (center: CGPoint, width: CGFloat, height: CGFloat) {
    let childBlocks = box.containedBlockIds.compactMap { blocksMap[$0] }
    if childBlocks.isEmpty {
      return (CGPoint(x: box.centerX + canvasPadding + 40, y: originY + box.centerY), box.width, box.height)
    }
    var minX: CGFloat = 1e9
    var maxX: CGFloat = -1e9
    var minY: CGFloat = 1e9
    var maxY: CGFloat = -1e9
    for b in childBlocks {
      let p = currentBlockPosition(b: b, originY: originY)
      minX = min(minX, p.x - b.width / 2.0)
      maxX = max(maxX, p.x + b.width / 2.0)
      minY = min(minY, p.y - b.height / 2.0)
      maxY = max(maxY, p.y + b.height / 2.0)
    }
    minX -= 14.0
    maxX += 14.0
    minY -= 12.0
    maxY += 12.0

    let center = CGPoint(x: (minX + maxX) / 2.0, y: (minY + maxY) / 2.0)
    let w = max(box.width, maxX - minX)
    let h = max(40, maxY - minY)
    return (center, w, h)
  }

  private func dynamicCanvasSize(data: (blocks: [GraphBlock], boxes: [ContainerBox], arrows: [GraphArrow], bounds: CGRect), blocksMap: [String: GraphBlock], originY: CGFloat) -> CGSize {
    let initialW = data.bounds.width
    let initialH = data.bounds.height

    var maxX = initialW - canvasPadding - 60
    var maxY = initialH - canvasPadding - 40

    for b in data.blocks {
      let pos = currentBlockPosition(b: b, originY: originY)
      let halfW = b.width / 2
      let halfH = b.height / 2

      maxX = max(maxX, pos.x + halfW + 60)
      maxY = max(maxY, pos.y + halfH + 40)
    }

    let calculatedW = max(initialW, maxX + canvasPadding)
    let calculatedH = max(initialH, maxY + canvasPadding)

    return CGSize(width: calculatedW, height: calculatedH)
  }

  // MARK: - View Body

  var body: some View {
    let data = graphData
    let originY = data.bounds.height / 2 + titleHeaderHeight / 2
    let blocksMap = Dictionary(uniqueKeysWithValues: data.blocks.map { ($0.id, $0) })
    let canvasSize = dynamicCanvasSize(data: data, blocksMap: blocksMap, originY: originY)

    VStack(alignment: .leading, spacing: 12) {
      // Card Title Header & Reset Button
      HStack(spacing: 8) {
        Image(systemName: "point.3.filled.connected.trianglepath.dotted")
          .font(.title3)
          .foregroundStyle(Color.accentColor)
        Text("DSP Signal Processing Graph")
          .font(.headline)

        if !customPositions.isEmpty {
          Button {
            withAnimation(.spring(response: 0.3, dampingFraction: 0.7)) {
              customPositions.removeAll()
            }
          } label: {
            HStack(spacing: 4) {
              Image(systemName: "arrow.counterclockwise")
              Text("Reset Layout")
            }
            .font(.caption)
            .foregroundStyle(Color.secondary)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(Color.primary.opacity(0.06))
            .clipShape(Capsule())
          }
          .buttonStyle(.plain)
        }

        Spacer()

        Text("\(pipeline.stages.filter(\.isEnabled).count) Active Stages")
          .font(.caption)
          .foregroundStyle(.secondary)
          .padding(.horizontal, 8)
          .padding(.vertical, 3)
          .background(Color.primary.opacity(0.06))
          .clipShape(Capsule())
      }

      // 2D Canvas View
      HorizontalScrollWithVerticalWheel {
        ZStack(alignment: .topLeading) {
          // Layer 1: Container Box Bounding Outlines (Dynamically Scaled)
          containerBoxesLayer(boxes: data.boxes, blocksMap: blocksMap, originY: originY)

          // Layer 2: Interactive Connecting Arrows Canvas
          arrowsCanvasLayer(arrows: data.arrows, blocksMap: blocksMap, originY: originY)

          // Layer 3: Draggable Interactive Blocks & Channel Ports
          blocksLayer(blocks: data.blocks, originY: originY)
        }
        .frame(width: canvasSize.width, height: canvasSize.height)
      }
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }

  // MARK: - Render Layers

  @ViewBuilder
  private func containerBoxesLayer(boxes: [ContainerBox], blocksMap: [String: GraphBlock], originY: CGFloat) -> some View {
    ZStack(alignment: .topLeading) {
      ForEach(boxes) { box in
        let frame = boxFrame(box: box, blocksMap: blocksMap, originY: originY)

        // Dashed container box centered directly over live channel ports
        RoundedRectangle(cornerRadius: 10)
          .fill(Color.primary.opacity(0.03))
          .overlay(
            RoundedRectangle(cornerRadius: 10)
              .stroke(Color.primary.opacity(0.18), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
          )
          .frame(width: frame.width, height: frame.height)
          .position(x: frame.center.x, y: frame.center.y)

        // Stage title header floated directly above container box
        Text(box.label)
          .font(.system(size: 11, weight: .bold, design: .monospaced))
          .foregroundStyle(Color.accentColor)
          .lineLimit(1)
          .position(x: frame.center.x, y: frame.center.y - (frame.height / 2) - 12)
      }
    }
  }

  @ViewBuilder
  private func blocksLayer(blocks: [GraphBlock], originY: CGFloat) -> some View {
    ZStack(alignment: .topLeading) {
      ForEach(blocks) { b in
        let currentPos = currentBlockPosition(b: b, originY: originY)

        Group {
          if b.isChannelPort {
            Text(b.label)
              .font(.system(size: 11, weight: .bold, design: .monospaced))
              .foregroundStyle(Color.blue)
              .frame(width: b.width, height: b.height)
              .background(Color.blue.opacity(0.12))
              .cornerRadius(6)
              .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.blue.opacity(0.3), lineWidth: 1))
          } else if !b.label.isEmpty {
            Text(b.label)
              .font(.system(size: 10, weight: .semibold, design: .monospaced))
              .foregroundStyle(.primary)
              .lineLimit(1)
              .padding(.horizontal, 4)
              .frame(width: b.width, height: b.height)
              .background(Color(nsColor: .controlBackgroundColor))
              .cornerRadius(6)
              .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.primary.opacity(0.25), lineWidth: 1))
          }
        }
        .position(x: currentPos.x, y: currentPos.y)
        .gesture(
          DragGesture(minimumDistance: 1)
            .onChanged { value in
              activeDragOffset = (id: b.id, translation: value.translation)
            }
            .onEnded { value in
              let basePos = customPositions[b.id] ?? CGPoint(x: b.x + canvasPadding + 40, y: originY + b.y)
              customPositions[b.id] = CGPoint(x: basePos.x + value.translation.width, y: basePos.y + value.translation.height)
              activeDragOffset = nil
            }
        )
      }
    }
  }

  private func arrowsCanvasLayer(arrows: [GraphArrow], blocksMap: [String: GraphBlock], originY: CGFloat) -> some View {
    Canvas { context, size in
      for arrow in arrows {
        let p0: CGPoint = {
          if let srcBlock = blocksMap[arrow.fromBlockId] {
            let pos = currentBlockPosition(b: srcBlock, originY: originY)
            return CGPoint(x: pos.x + srcBlock.width / 2, y: pos.y)
          }
          return CGPoint(x: arrow.fromFallback.x + canvasPadding + 40, y: originY + arrow.fromFallback.y)
        }()

        let p1: CGPoint = {
          if let destBlock = blocksMap[arrow.toBlockId] {
            let pos = currentBlockPosition(b: destBlock, originY: originY)
            return CGPoint(x: pos.x - destBlock.width / 2, y: pos.y)
          }
          return CGPoint(x: arrow.toFallback.x + canvasPadding + 40, y: originY + arrow.toFallback.y)
        }()

        let dx = p1.x - p0.x
        let ctrl1 = CGPoint(x: p0.x + dx * 0.45, y: p0.y)
        let ctrl2 = CGPoint(x: p0.x + dx * 0.55, y: p1.y)

        var path = Path()
        path.move(to: p0)
        path.addCurve(to: p1, control1: ctrl1, control2: ctrl2)

        context.stroke(path, with: .color(Color.gray.opacity(0.6)), lineWidth: 1.2)

        // Draw Arrowhead cap
        var arrowCap = Path()
        arrowCap.move(to: p1)
        arrowCap.addLine(to: CGPoint(x: p1.x - 6, y: p1.y - 3.5))
        arrowCap.addLine(to: CGPoint(x: p1.x - 6, y: p1.y + 3.5))
        arrowCap.closeSubpath()
        context.fill(arrowCap, with: .color(Color.gray.opacity(0.8)))

        // Draw gain label along arrow
        if let label = arrow.label {
          let midPoint = CGPoint(x: p0.x + dx * 0.65, y: p0.y + (p1.y - p0.y) * 0.65)
          context.draw(
            Text(label)
              .font(.system(size: 8, weight: .semibold, design: .monospaced))
              .foregroundColor(Color.secondary),
            at: midPoint
          )
        }
      }
    }
  }

  // MARK: - Readable Formatting Helpers

  private func readableMixerTitle(_ rawName: String, inCh: Int, outCh: Int) -> String {
    if rawName.contains("2to4") { return "2to4" }
    if rawName.contains("4to2") { return "4to2" }
    if !rawName.isEmpty { return rawName }
    return "\(inCh)to\(outCh)"
  }

  private func readableFilterStepName(_ rawName: String, stage: PipelineStage) -> String {
    if rawName.contains("cx5_hi") { return "cx5_hi" }
    if rawName.contains("cx5_lo_gain") { return "cx5_lo_gain" }
    if rawName.contains("cx5_lo") { return "cx5_lo" }

    let parts = rawName.components(separatedBy: "_")
    guard let suffix = parts.last else { return rawName }

    switch suffix {
    case "preamp": return "preamp"
    case "invert": return "invert"
    case "lp": return "Linkwitz_LP"
    case "hp": return "Linkwitz_HP"
    case "conv": return "Convolution"
    case "gain": return "Gain"
    case "delay": return "Delay"
    default:
      return rawName
    }
  }
}
