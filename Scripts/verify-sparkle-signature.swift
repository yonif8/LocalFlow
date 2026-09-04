#!/usr/bin/env swift

import CryptoKit
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("error: \(message)\n".utf8))
    exit(1)
}

guard CommandLine.arguments.count == 4 else {
    fail("usage: verify-sparkle-signature.swift <archive> <base64-signature> <base64-public-key-file>")
}

let archiveURL = URL(fileURLWithPath: CommandLine.arguments[1])
let signatureText = CommandLine.arguments[2]
let publicKeyURL = URL(fileURLWithPath: CommandLine.arguments[3])

let archive: Data
let publicKeyText: String
do {
    archive = try Data(contentsOf: archiveURL, options: [.mappedIfSafe])
    publicKeyText = try String(contentsOf: publicKeyURL, encoding: .utf8)
        .filter { !$0.isWhitespace }
} catch {
    fail("could not read release inputs: \(error.localizedDescription)")
}

guard let signature = Data(base64Encoded: signatureText), signature.count == 64 else {
    fail("the Sparkle Ed25519 signature is not valid base64 or is not 64 bytes")
}
guard let publicKeyData = Data(base64Encoded: publicKeyText), publicKeyData.count == 32 else {
    fail("the Sparkle Ed25519 public key is not valid base64 or is not 32 bytes")
}

do {
    let publicKey = try Curve25519.Signing.PublicKey(rawRepresentation: publicKeyData)
    guard publicKey.isValidSignature(signature, for: archive) else {
        fail("the Sparkle signature does not verify against the pinned public key")
    }
} catch {
    fail("could not verify the Sparkle signature: \(error.localizedDescription)")
}

print("Sparkle Ed25519 signature verified.")
