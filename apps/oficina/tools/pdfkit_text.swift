// The text of a PDF as macOS's PDFKit reads it: a second opinion on the
// PDFs Office writes and on the samples of tools/samples_pdf.py.
//
//     swiftc -O apps/oficina/tools/pdfkit_text.swift -o /tmp/pdfkit_text
//     /tmp/pdfkit_text file.pdf [password]
import PDFKit
import Foundation
let a = CommandLine.arguments
guard let d = PDFDocument(url: URL(fileURLWithPath: a[1])) else { print("PDFKit: cannot open"); exit(1) }
if d.isLocked { let ok = d.unlock(withPassword: a.count > 2 ? a[2] : ""); if !ok { print("PDFKit: locked"); exit(1) } }
print("PDFKit pages:", d.pageCount)
for i in 0..<d.pageCount { print(d.page(at: i)?.string ?? "") }
