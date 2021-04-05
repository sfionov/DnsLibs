// swift-tools-version:5.3
import PackageDescription

let package = Package(
  name: "AGDnsProxy",
  platforms: [
    .iOS("10.0"), .macOS("10.12")
  ],
  products: [
    .library(name: "AGDnsProxy", targets: ["AGDnsProxy"]),
  ],
  targets: [
    .binaryTarget(
      name: "AGDnsProxy",
      url: "https://github.com/sfionov/DnsLibs/releases/download/v1.5.5-spm/AGDnsProxy-1.5.5-spm.zip",
      checksum: "68aa0110a8596cb3f2748e9bbb8cebc9c4d8453a3177387a33f157a28ffb051e"
    ),
  ]
)
