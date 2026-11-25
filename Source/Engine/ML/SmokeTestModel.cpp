#include "InferenceBackend.h"

// Minimal protobuf writer, just enough to emit an ONNX ModelProto.
namespace ve::ml
{
    namespace
    {
        struct Pb
        {
            std::vector<std::uint8_t> out;

            void varint (std::uint64_t v)
            {
                while (v >= 0x80) { out.push_back ((std::uint8_t) ((v & 0x7f) | 0x80)); v >>= 7; }
                out.push_back ((std::uint8_t) v);
            }
            void key (int field, int wire) { varint ((std::uint64_t) ((field << 3) | wire)); }
            void intField (int field, std::int64_t v) { key (field, 0); varint ((std::uint64_t) v); }
            void bytesField (int field, const void* d, size_t n) { key (field, 2); varint (n); auto* p = static_cast<const std::uint8_t*> (d); out.insert (out.end(), p, p + n); }
            void stringField (int field, const juce::String& s)
            {
                const auto utf8 = s.toStdString();
                bytesField (field, utf8.data(), utf8.size());
            }
            void messageField (int field, const Pb& m) { bytesField (field, m.out.data(), m.out.size()); }
        };

        Pb valueInfo (const juce::String& name, std::int64_t dim)
        {
            Pb d;          d.intField (1, dim);              // Dimension.dim_value
            Pb shape;      shape.messageField (1, d);        // TensorShapeProto.dim
            Pb tensor;     tensor.intField (1, 1);           // elem_type = FLOAT
                           tensor.messageField (2, shape);   // shape
            Pb type;       type.messageField (1, tensor);    // TypeProto.tensor_type
            Pb vi;         vi.stringField (1, name);         // ValueInfoProto.name
                           vi.messageField (2, type);        // type
            return vi;
        }
    }

    juce::MemoryBlock makeSmokeTestModel()
    {
        Pb node;
        node.stringField (1, "X");     // input
        node.stringField (1, "X");     // input
        node.stringField (2, "Y");     // output
        node.stringField (3, "add");   // name
        node.stringField (4, "Add");   // op_type

        Pb graph;
        graph.messageField (1, node);              // node
        graph.stringField (2, "ve_smoke");         // name
        graph.messageField (11, valueInfo ("X", 4)); // input
        graph.messageField (12, valueInfo ("Y", 4)); // output

        Pb opset;
        opset.stringField (1, "");   // domain
        opset.intField (2, 13);      // version

        Pb model;
        model.intField (1, 8);                  // ir_version
        model.stringField (2, "VocalEngine");   // producer_name
        model.messageField (7, graph);          // graph
        model.messageField (8, opset);          // opset_import

        return juce::MemoryBlock (model.out.data(), model.out.size());
    }
}
