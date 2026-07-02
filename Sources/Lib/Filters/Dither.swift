import DSPAudio
import DSPConfig
import Foundation

// MARK: - Ditherers

private struct NoopDitherer {
  init(amplitude _: PrcFmt) {}
  mutating func sample() -> PrcFmt { 0.0 }
}

private struct TriangularDitherer {
  private var halfAmplitude: PrcFmt

  init(amplitude: PrcFmt) {
    self.halfAmplitude = amplitude / 2.0
  }

  mutating func sample() -> PrcFmt {
    let u = PrcFmt.random(in: 0.0...1.0)
    let a = -halfAmplitude
    let b = halfAmplitude
    let c: PrcFmt = 0.0
    let fc = (c - a) / (b - a)

    if u < fc {
      return a + sqrt(u * (b - a) * (c - a))
    } else {
      return b - sqrt((1.0 - u) * (b - a) * (b - c))
    }
  }
}

private struct HighpassDitherer {
  private var halfAmplitude: PrcFmt
  private var previousSample: PrcFmt = 0.0

  init(amplitude: PrcFmt) {
    self.halfAmplitude = amplitude / 2.0
  }

  mutating func sample() -> PrcFmt {
    let newSample = PrcFmt.random(in: -halfAmplitude...halfAmplitude)
    let highPassed = newSample - previousSample
    previousSample = newSample
    return highPassed
  }
}

// MARK: - NoiseShaper

private final class NoiseShaper {
  let filter: [PrcFmt]
  private var buffer: [PrcFmt]
  private var writeIndex: Int

  init(filter: [PrcFmt]) {
    self.filter = filter
    self.buffer = [PrcFmt](repeating: 0.0, count: filter.count)
    self.writeIndex = 0
  }

  func process(scaled: PrcFmt, dither: PrcFmt) -> PrcFmt {
    var filtBuf: PrcFmt = 0.0
    let count = filter.count
    for i in 0..<count {
      let bufIdx = (writeIndex + i) % count
      let coeffIdx = count - 1 - i
      filtBuf += filter[coeffIdx] * buffer[bufIdx]
    }

    let scaledPlusErr = scaled + filtBuf
    let result = scaledPlusErr + dither
    let resultR = result.rounded(.toNearestOrAwayFromZero)

    let error = scaledPlusErr - resultR
    buffer[writeIndex] = error
    writeIndex = (writeIndex + 1) % count

    return resultR
  }
}

// MARK: - Noise Shaper Factory

extension NoiseShaper {
  static func fweighted441() -> NoiseShaper {
    NoiseShaper(filter: [2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847])
  }

  static func fweightedLong441() -> NoiseShaper {
    NoiseShaper(filter: [
      2.391510, -3.284444, 3.679506, -3.635044, 2.524185, -1.146701, 0.115354, 0.513745,
      -0.749277, 0.512386, -0.749277, 0.512386, -0.188997, -0.043705, 0.149843, -0.151186,
      0.076302, -0.012070, -0.021127, 0.025232, -0.016121, 0.004453, 0.000876, -0.001799,
      0.000774, -0.000128,
    ])
  }

  static func fweightedShort441() -> NoiseShaper {
    NoiseShaper(filter: [1.623, -0.982, 0.109])
  }

  static func gesemann441() -> NoiseShaper {
    NoiseShaper(filter: [2.2061, -0.4706, -0.2534, -0.6214, 1.0587, 0.0676, -0.6054, -0.2738])
  }

  static func gesemann48() -> NoiseShaper {
    NoiseShaper(filter: [2.2374, -0.7339, -0.1251, -0.6033, 0.903, 0.0116, -0.5853, -0.2571])
  }

  static func lipshitz441() -> NoiseShaper {
    NoiseShaper(filter: [2.033, -2.165, 1.959, -1.590, 0.6149])
  }

  static func lipshitzLong441() -> NoiseShaper {
    NoiseShaper(filter: [2.847, -4.685, 6.214, -7.184, 6.639, -5.032, 3.263, -1.632, 0.4191])
  }

  static func shibata441() -> NoiseShaper {
    NoiseShaper(filter: [
      1.356_863_856_315_612_8, -1.225_293_517_112_732, 0.623_555_064_201_355,
      -0.225_620_940_327_644_35, -0.235_579_758_882_522_58, 0.135_363_623_499_870_3,
      -0.091_538_146_138_191_22, -0.056_445_639_580_488_205, 3.961_442_416_766_658_4e-5,
      -0.023_561_919_108_033_18, -0.010_756_319_388_747_215, -0.000_319_491_315_167_397_26,
      0.001_433_762_023_225_426_7, -0.008_455_123_752_355_576, -0.000_213_181_803_701_445_46,
      7.617_592_200_404_033e-5, 0.001_010_233_070_701_360_7, 4.503_027_594_182_64e-5,
      0.001_343_382_173_217_833, 0.001_393_724_232_912_063_6, 0.000_433_067_005_360_499,
      0.000_469_497_870_653_867_7, 0.000_147_758_415_550_924_84, -4.106_017_513_549_886_6e-5,
    ])
  }

  static func shibataHigh441() -> NoiseShaper {
    NoiseShaper(filter: [
      2.826_326_608_657_837, -5.353_435_993_194_58, 7.804_205_894_470_215,
      -9.679_368_972_778_32, 10.157135009765625, -9.439_995_765_686_035,
      7.614_612_579_345_703, -5.424_517_631_530_762, 3.247_828_245_162_964,
      -1.630_185_246_467_590_3, 0.585_380_196_571_350_1, -0.117_100_022_733_211_52,
      -0.033_543_668_687_343_6, 0.008_884_146_809_577_942, 0.017_314_357_683_062_553,
      -0.033_262_729_644_775_39, 0.018_168_220_296_502_113, -0.006_801_502_779_126_167,
      -0.000_969_119_486_398_994_9, 0.000_964_893_435_593_694_4,
    ])
  }

  static func shibataLow441() -> NoiseShaper {
    NoiseShaper(filter: [
      0.595_437_824_726_104_7, -0.002_507_873_112_335_801, -0.185_180_589_556_694_03,
      -0.001_037_429_319_694_638_3, -0.103_663_429_617_881_77, -0.053_248_628_973_960_876,
      -8.403_004_903_811_961e-5, -3.856_993_302_520_095e-8, -0.026_413_010_433_316_23,
      -0.000_684_383_965_563_029, 3.158_050_503_770_937_2e-6, 0.031_739_629_805_088_04,
    ])
  }

  static func shibata48() -> NoiseShaper {
    NoiseShaper(filter: [
      1.491_957_783_699_035_6, -1.308_917_880_058_288_6, 0.540_516_316_890_716_6,
      -0.000_361_137_499_567_121_27, -0.363_031_953_573_226_93, 0.109_111_279_249_191_28,
      0.007_310_638_204_216_957, -0.115_459_144_115_448, 0.003_772_285_534_068_942,
      -0.012_545_258_738_100_529, -0.029_272_487_387_061_12, -0.005_002_200_137_823_82,
      -0.000_202_188_515_686_430_04, -0.004_905_734_676_867_723_5, -0.005_127_976_182_848_215,
      -0.002_505_671_000_108_123,
    ])
  }

  static func shibataHigh48() -> NoiseShaper {
    NoiseShaper(filter: [
      3.260_151_624_679_565_4, -6.557_569_503_784_18, 9.748_664_855_957_031,
      -11.713_088_989_257_813, 11.504_628_181_457_52, -9.485_962_867_736_816,
      6.404_273_033_142_09, -3.477_282_047_271_728_5, 1.332_738_280_296_325_7,
      -0.264_645_755_290_985_1, -0.081_823_304_295_539_86, 0.044_643_409_550_189_97,
      0.021_642_472_594_976_425, -0.042_832_121_253_013_61, 0.003_383_262_082_934_379_6,
      0.016_050_558_537_244_797, -0.019_443_769_007_921_22, 0.002_014_045_603_573_322_3,
      0.005_101_846_531_033_516, -0.004_944_144_282_490_015, -0.001_399_693_894_200_027,
      0.003_581_011_900_678_277, -0.002_209_919_737_651_944, -0.000_101_200_050_266_925_25,
      0.000_771_208_666_265_010_8, -4.772_754_982_695_915e-5, -0.000_470_578_757_813_200_35,
      0.000_535_220_140_591_263_8,
    ])
  }

  static func shibataLow48() -> NoiseShaper {
    NoiseShaper(filter: [
      0.648_154_377_937_316_9, -0.000_132_923_290_948_383_5, -0.152_844_399_213_790_9,
      -0.024_795_081_466_436_386, -0.028_879_294_171_929_36, -0.097_741_305_828_094_48,
      3.723_334_521_055_221_6e-5, 3.036_181_624_338_496_5e-6, -2.685_151_775_949_634_6e-5,
      -0.015_118_855_983_018_875, -0.000_119_081_560_114_864_26, 4.020_391_770_609_422e-6,
      0.032_142_307_609_319_69,
    ])
  }

  static func shibata882() -> NoiseShaper {
    NoiseShaper(filter: [
      2.075_203_657_150_268_6, -1.431_611_061_096_191_4, -4.101_862_214_156_426_5e-5,
      0.307_477_861_642_837_5, 0.015_034_947_544_336_319, -0.002_069_007_372_483_611,
      -0.095_445_446_670_055_39, -0.017_573_365_941_643_715, 0.001_514_684_408_903_122,
      0.009_715_720_079_839_23, 0.003_230_015_747_249_126_4, -0.001_166_222_151_368_856_4,
      -0.012_702_429_667_115_211, -0.013_680_535_368_621_35, -0.000_326_957_117_067_649_96,
      -0.000_334_812_386_427_074_67, 0.001_941_891_969_181_597_2, -0.006_559_844_594_448_805,
      -0.003_184_868_488_460_779, -0.001_185_707_631_520_927,
    ])
  }

  static func shibataLow882() -> NoiseShaper {
    NoiseShaper(filter: [
      0.812_750_816_345_214_8, 1.341_541_633_337_328_7e-7, -1.400_316_978_106_275_2e-5,
      -0.027_366_658_672_690_39, -0.063_084_796_071_052_55, -0.000_411_249_639_000_743_63,
      -0.001_466_781_133_785_843_8, -0.003_463_642_438_873_648_6, -0.014_447_951_689_362_526,
      -0.050_686_400_383_710_86,
    ])
  }

  static func shibata96() -> NoiseShaper {
    NoiseShaper(filter: [
      2.104_111_433_029_175, -1.410_141_706_466_674_8, -0.003_514_738_753_437_996,
      0.186_179_712_414_741_52, 0.111_176_766_455_173_49, -0.001_362_945_069_558_918_5,
      -0.055_446_717_888_116_84, -0.056_859_914_213_418_96, -0.003_957_323_264_330_625_5,
      0.002_566_334_791_481_495, 0.014_090_753_160_417_08, 0.006_225_708_406_418_562,
      -0.006_539_735_011_756_42, -0.019_066_527_485_847_473,
    ])
  }

  static func shibataLow96() -> NoiseShaper {
    NoiseShaper(filter: [
      0.833_627_820_014_953_6, 4.766_351_082_707_842_6e-7, -5.592_720_481_217_839e-5,
      -0.000_917_676_079_552_620_6, -0.085_019_297_897_815_7, -0.000_308_640_970_615_670_1,
      -2.747_484_904_830_344e-5, -3.447_055_496_508_255_6e-5, -0.006_816_617_213_189_602,
      -0.005_103_240_255_266_428, -0.048_310_291_022_062_3,
    ])
  }

  static func shibata192() -> NoiseShaper {
    NoiseShaper(filter: [
      2.117_482_662_200_927_7, -0.793_001_294_136_047_4, -0.588_716_506_958_007_8,
      -0.004_517_062_101_513_147, -2.240_059_620_817_192e-5, 0.349_810_659_885_406_5,
      0.001_467_469_963_245_093_8, -0.035_286_050_289_869_31, -0.030_574_915_930_628_777,
      -0.008_099_924_772_977_829, -0.024_920_884_519_815_445, -0.010_276_389_308_273_792,
      -0.002_827_338_874_340_057_4, 0.011_965_871_788_561_344,
    ])
  }

  static func shibataLow192() -> NoiseShaper {
    NoiseShaper(filter: [
      0.929_867_863_655_090_3, 2.375_700_432_821_759e-6, 1.323_920_400_864_153_6e-6,
      4.533_644_570_869_91e-8, -1.085_569_920_178_386_4e-6, -7.519_394_671_362_534e-7,
      -0.010_574_714_280_664_92, -0.015_397_379_174_828_53, -0.007_173_464_633_524_418,
      -0.004_041_632_637_381_554,
    ])
  }
}

// MARK: - DitherFilter

final class DitherFilter: Filter {
  let name: String
  private var scalefact: PrcFmt
  private var shaper: NoiseShaper?

  private enum DithererKind {
    case noop(NoopDitherer)
    case triangular(TriangularDitherer)
    case highpass(HighpassDitherer)

    mutating func sample() -> PrcFmt {
      switch self {
      case .noop(var d):
        let v = d.sample()
        self = .noop(d)
        return v
      case .triangular(var d):
        let v = d.sample()
        self = .triangular(d)
        return v
      case .highpass(var d):
        let v = d.sample()
        self = .highpass(d)
        return v
      }
    }
  }

  private var ditherer: DithererKind

  init(name: String = "dither", parameters: DitherParameters) {
    self.name = name
    let bits = parameters.bits
    let ditherType = parameters.type
    self.scalefact = pow(2.0, PrcFmt(bits - 1))

    self.shaper = Self.makeShaper(for: ditherType)

    let ditherer: DithererKind
    switch ditherType {
    case .none:
      ditherer = .noop(NoopDitherer(amplitude: 0.0))
    case .flat:
      let amplitude = parameters.amplitude ?? 2.0
      ditherer = .triangular(TriangularDitherer(amplitude: amplitude))
    case .highpass:
      ditherer = .highpass(HighpassDitherer(amplitude: 2.0))
    default:
      ditherer = .triangular(TriangularDitherer(amplitude: 2.0))
    }
    self.ditherer = ditherer
  }

  func process(waveform: MutableWaveform) {
    for i in 0..<waveform.count {
      let scaled = waveform[i] * scalefact
      let dither = ditherer.sample()

      let resultR: PrcFmt
      if let s = shaper {
        resultR = s.process(scaled: scaled, dither: dither)
      } else {
        let result = scaled + dither
        resultR = result.rounded(.toNearestOrAwayFromZero)
      }

      waveform[i] = resultR / scalefact
    }
  }

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .dither(let params) = config else { return }
    let bits = params.bits
    let ditherType = params.type
    self.scalefact = pow(2.0, PrcFmt(bits - 1))

    self.shaper = Self.makeShaper(for: ditherType)

    switch ditherType {
    case .none:
      self.ditherer = .noop(NoopDitherer(amplitude: 0.0))
    case .flat:
      let amplitude = params.amplitude ?? 2.0
      self.ditherer = .triangular(TriangularDitherer(amplitude: amplitude))
    case .highpass:
      self.ditherer = .highpass(HighpassDitherer(amplitude: 2.0))
    default:
      self.ditherer = .triangular(TriangularDitherer(amplitude: 2.0))
    }
  }

  private static func makeShaper(for ditherType: DitherType) -> NoiseShaper? {
    switch ditherType {
    case .none, .flat, .highpass:
      return nil
    case .fweighted441:
      return .fweighted441()
    case .fweightedLong441:
      return .fweightedLong441()
    case .fweightedShort441:
      return .fweightedShort441()
    case .gesemann441:
      return .gesemann441()
    case .gesemann48:
      return .gesemann48()
    case .lipshitz441:
      return .lipshitz441()
    case .lipshitzLong441:
      return .lipshitzLong441()
    case .shibata441:
      return .shibata441()
    case .shibataHigh441:
      return .shibataHigh441()
    case .shibataLow441:
      return .shibataLow441()
    case .shibata48:
      return .shibata48()
    case .shibataHigh48:
      return .shibataHigh48()
    case .shibataLow48:
      return .shibataLow48()
    case .shibata882:
      return .shibata882()
    case .shibataLow882:
      return .shibataLow882()
    case .shibata96:
      return .shibata96()
    case .shibataLow96:
      return .shibataLow96()
    case .shibata192:
      return .shibata192()
    case .shibataLow192:
      return .shibataLow192()
    }
  }
}
