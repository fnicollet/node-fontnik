#include "glyphs.hpp"
#include "font_face_set.hpp"
#include "harfbuzz_shaper.hpp"
#include "tile_face.hpp"

// node
#include <node_buffer.h>

// stl
#include <set>
#include <algorithm>
#include <memory>
#include <sstream>
#include <iostream>

// freetype2
extern "C"
{
#include <ft2build.h>
#include FT_FREETYPE_H
// #include FT_STROKER_H
}

struct RangeBaton {
    v8::Persistent<v8::Function> callback;
    Glyphs *glyphs;
    std::string fontstack;
    std::string range;
    bool error;
    std::string error_name;
    std::vector<std::uint32_t> chars;
};

struct ShapeBaton {
    v8::Persistent<v8::Function> callback;
    Glyphs *glyphs;
    std::string fontstack;
};

v8::Persistent<v8::FunctionTemplate> Glyphs::constructor;

Glyphs::Glyphs() : node::ObjectWrap() {}

Glyphs::Glyphs(const char *data, size_t length, bool isTile) : node::ObjectWrap() {
    isTile ? tile.ParseFromArray(data, length) : glyphs.ParseFromArray(data, length);
}

Glyphs::~Glyphs() {}

void Glyphs::Init(v8::Handle<v8::Object> target) {
    v8::HandleScope scope;

    v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New(New);
    v8::Local<v8::String> name = v8::String::NewSymbol("Glyphs");

    constructor = v8::Persistent<v8::FunctionTemplate>::New(tpl);

    // node::ObjectWrap uses the first internal field to store the wrapped pointer.
    constructor->InstanceTemplate()->SetInternalFieldCount(1);
    constructor->SetClassName(name);

    // Add all prototype methods, getters and setters here.
    NODE_SET_PROTOTYPE_METHOD(constructor, "serialize", Serialize);
    NODE_SET_PROTOTYPE_METHOD(constructor, "serializeTile", SerializeTile);
    NODE_SET_PROTOTYPE_METHOD(constructor, "range", Range);
    NODE_SET_PROTOTYPE_METHOD(constructor, "shape", Shape);

    // This has to be last, otherwise the properties won't show up on the
    // object in JavaScript.
    target->Set(name, constructor->GetFunction());
}

v8::Handle<v8::Value> Glyphs::New(const v8::Arguments& args) {
    if (!args.IsConstructCall()) {
        return ThrowException(v8::Exception::TypeError(v8::String::New("Constructor must be called with new keyword")));
    }
    if (args.Length() > 0 && !node::Buffer::HasInstance(args[0])) {
        return ThrowException(v8::Exception::TypeError(v8::String::New("First argument may only be a buffer")));
    }
    if (args.Length() > 1 && !args[1]->IsBoolean()) {
        return ThrowException(v8::Exception::TypeError(v8::String::New("Second argument may only be a boolean")));
    }

    Glyphs* glyphs;

    if (args.Length() < 1) {
        glyphs = new Glyphs();
    } else {
        v8::Local<v8::Object> buffer = args[0]->ToObject();
        bool isTile = args[1]->BooleanValue() || false;
        glyphs = new Glyphs(node::Buffer::Data(buffer), node::Buffer::Length(buffer), isTile);
    }
    
    glyphs->Wrap(args.This());

    return args.This();
}

bool Glyphs::HasInstance(v8::Handle<v8::Value> val) {
    if (!val->IsObject()) return false;
    return constructor->HasInstance(val->ToObject());
}

v8::Handle<v8::Value> Glyphs::Serialize(const v8::Arguments& args) {
    v8::HandleScope scope;
    llmr::glyphs::glyphs& glyphs = node::ObjectWrap::Unwrap<Glyphs>(args.This())->glyphs;
    std::string serialized = glyphs.SerializeAsString();
    return scope.Close(node::Buffer::New(serialized.data(), serialized.length())->handle_);
}

v8::Handle<v8::Value> Glyphs::SerializeTile(const v8::Arguments& args) {
    v8::HandleScope scope;
    llmr::vector::tile& tile = node::ObjectWrap::Unwrap<Glyphs>(args.This())->tile;
    std::string serialized = tile.SerializeAsString();
    return scope.Close(node::Buffer::New(serialized.data(), serialized.length())->handle_);
}

v8::Handle<v8::Value> Glyphs::Range(const v8::Arguments& args) {
    v8::HandleScope scope;

    // Validate arguments.
    if (args.Length() < 1 || !args[0]->IsString()) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("fontstack must be a string")));
    }

    if (args.Length() < 2 || !args[1]->IsString()) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("range must be a string")));
    }

    if (args.Length() < 3 || !args[2]->IsArray()) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("chars must be an array")));
    }

    if (args.Length() < 4 || !args[3]->IsFunction()) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("callback must be a function")));
    }

    v8::String::Utf8Value fontstack(args[0]->ToString());
    v8::String::Utf8Value range(args[1]->ToString());
    v8::Local<v8::Array> charsArray = v8::Local<v8::Array>::Cast(args[2]);
    v8::Local<v8::Function> callback = v8::Local<v8::Function>::Cast(args[3]);

    unsigned array_size = charsArray->Length();
    std::vector<std::uint32_t> chars;
    for (unsigned i=0; i < array_size; i++) {
        chars.push_back(charsArray->Get(i)->IntegerValue());
    }

    Glyphs *glyphs = node::ObjectWrap::Unwrap<Glyphs>(args.This());

    RangeBaton* baton = new RangeBaton();
    baton->callback = v8::Persistent<v8::Function>::New(callback);
    baton->glyphs = glyphs;
    baton->fontstack = *fontstack;
    baton->range = *range;
    baton->chars = chars;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    int status = uv_queue_work(uv_default_loop(), req, AsyncRange, (uv_after_work_cb)RangeAfter);
    assert(status == 0);

    return v8::Undefined();
}

void Glyphs::AsyncRange(uv_work_t* req) {
    RangeBaton* baton = static_cast<RangeBaton*>(req->data);

    fontserver::freetype_engine font_engine_;
    fontserver::face_manager_freetype font_manager(font_engine_);

    fontserver::font_set fset(baton->fontstack);
    fset.add_fontstack(baton->fontstack, ',');

    fontserver::face_set_ptr face_set;

    try {
        face_set = font_manager.get_face_set(fset);
    } catch(const std::runtime_error &e) {
        baton->error = true;
        baton->error_name = e.what();
        return;
    }

    llmr::glyphs::glyphs& glyphs = baton->glyphs->glyphs;

    llmr::glyphs::fontstack *mutable_fontstack = glyphs.add_stacks();
    mutable_fontstack->set_name(baton->fontstack);
    mutable_fontstack->set_range(baton->range);

    fontserver::text_format format(baton->fontstack, 24);
    const double scale_factor = 1.0;

    // Set character sizes.
    double size = format.text_size * scale_factor;
    face_set->set_character_sizes(size);

    for (std::vector<uint32_t>::size_type i = 0; i != baton->chars.size(); i++) {
        FT_ULong char_code = baton->chars[i];
        fontserver::glyph_info glyph;

        for (auto const& face : *face_set) {
            // Get FreeType face from face_ptr.
            FT_Face ft_face = face->get_face();
            FT_UInt char_index = FT_Get_Char_Index(ft_face, char_code);

            // Try next font in fontset.
            if (!char_index) continue;

            glyph.glyph_index = char_index;
            face->glyph_dimensions(glyph);

            // Add glyph to fontstack.
            llmr::glyphs::glyph *mutable_glyph = mutable_fontstack->add_glyphs();
            mutable_glyph->set_id(char_code);
            mutable_glyph->set_width(glyph.width);
            mutable_glyph->set_height(glyph.height);
            mutable_glyph->set_left(glyph.left);
            mutable_glyph->set_top(glyph.top - glyph.ascender);
            mutable_glyph->set_advance(glyph.advance);

            if (glyph.width > 0) {
                mutable_glyph->set_bitmap(glyph.bitmap);
            }

            // Glyph added, continue to next char_code.
            break;
        }
    }
}

void Glyphs::RangeAfter(uv_work_t* req) {
    v8::HandleScope scope;
    RangeBaton* baton = static_cast<RangeBaton*>(req->data);

    const unsigned argc = 1;

    v8::TryCatch try_catch;

    if (baton->error) {
        v8::Local<v8::Value> argv[argc] = { v8::Exception::Error(v8::String::New(baton->error_name.c_str())) };
        baton->callback->Call(v8::Context::GetCurrent()->Global(), argc, argv);
    } else {
        v8::Local<v8::Value> argv[argc] = { v8::Local<v8::Value>::New(v8::Null()) };
        baton->callback->Call(v8::Context::GetCurrent()->Global(), argc, argv);
    }

    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }

    baton->callback.Dispose();

    delete baton;
    delete req;
}

v8::Handle<v8::Value> Glyphs::Shape(const v8::Arguments& args) {
    v8::HandleScope scope;

    if (args.Length() < 1) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("First argument must be a font stack")));
    }

    if (args.Length() < 2 || !args[1]->IsFunction()) {
        return ThrowException(v8::Exception::TypeError(
            v8::String::New("Second argument must be a callback function")));
    }
    v8::Local<v8::Function> callback = v8::Local<v8::Function>::Cast(args[1]);
    // TODO: validate this is a string
    v8::String::Utf8Value fontstack(args[0]->ToString());

    Glyphs *glyphs = node::ObjectWrap::Unwrap<Glyphs>(args.This());

    ShapeBaton* baton = new ShapeBaton();
    baton->callback = v8::Persistent<v8::Function>::New(callback);
    baton->glyphs = glyphs;
    baton->fontstack = *fontstack;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    int status = uv_queue_work(uv_default_loop(), req, AsyncShape, (uv_after_work_cb)ShapeAfter);
    assert(status == 0);

    return v8::Undefined();
}

void Glyphs::AsyncShape(uv_work_t* req) {
    ShapeBaton* baton = static_cast<ShapeBaton*>(req->data);
    // Maps char index (UTF-16) to width. If multiple glyphs map to the
    // same char the sum of all widths is used.
    // Note: this probably isn't the best solution. it would be better
    // to have an object for each cluster, but it needs to be
    // implemented with no overhead.
    std::map<unsigned, double> width_map_;
    fontserver::freetype_engine font_engine_;
    fontserver::face_manager_freetype font_manager(font_engine_);
    fontserver::text_itemizer itemizer;

    fontserver::font_set fset(baton->fontstack);
    fset.add_fontstack(baton->fontstack, ',');

    fontserver::face_set_ptr face_set = font_manager.get_face_set(fset);
    if (!face_set->size()) return;

    std::map<fontserver::face_ptr, fontserver::tile_face *> face_map;
    std::vector<fontserver::tile_face *> tile_faces;

    llmr::vector::tile& tile = baton->glyphs->tile;

    // for every label
    for (int i = 0; i < tile.layers_size(); i++) {
        const llmr::vector::layer& layer = tile.layers(i);

        typedef std::set<int> Strings;
        Strings strings;

        // Compile a set of all strings we need to shape.
        for (int j = 0; j < layer.features_size(); j++) {
            const llmr::vector::feature& feature = layer.features(j);

            for (int k = 1; k < feature.tags_size(); k += 2) {
                const std::string& key = layer.keys(feature.tags(k - 1));
                if (key == "name") {
                    // TODO: handle multiple fonts stacks
                    strings.insert(feature.tags(k));
                }
                // TODO: extract all keys we need to shape
            }
        }

        llmr::vector::layer* mutable_layer = tile.mutable_layers(i);

        fontserver::text_format format(baton->fontstack, 24);
        fontserver::text_format_ptr format_ptr = 
            std::make_shared<fontserver::text_format>(format);

        // Process strings per layer.
        for (auto const& key : strings) {
            const llmr::vector::value& value = layer.values(key);
            std::string text;
            if (value.has_string_value()) {
                text = value.string_value();
            }

            if (!text.empty()) {
                // Clear cluster widths.
                width_map_.clear();

                UnicodeString const& str = text.data();

                fontserver::text_line line(0, str.length() - 1);

                itemizer.add_text(str, format_ptr);

                const double scale_factor = 1.0;

                // Shape the text.
                fontserver::harfbuzz_shaper shaper;
                shaper.shape_text(line,
                                  itemizer,
                                  width_map_,
                                  face_set,
                                  // font_manager,
                                  scale_factor);

                llmr::vector::label *label = mutable_layer->add_labels();
                label->set_text(key);

                // TODO: support multiple font stacks
                label->set_stack(0); 

                // Add all glyphs for this labels and add new font
                // faces as they appear.
                for (auto const& glyph : line) {
                    if (!glyph.face) {
                        continue;
                    }

                    // Try to find whether this font has already been
                    // used in this tile.
                    std::map<fontserver::face_ptr, fontserver::tile_face *>::iterator face_map_itr = face_map.find(glyph.face);
                    if (face_map_itr == face_map.end()) {
                        fontserver::tile_face *face = 
                            new fontserver::tile_face(glyph.face);
                        std::pair<fontserver::face_ptr, fontserver::tile_face *> keyed(glyph.face, face);
                        face_map_itr = face_map.insert(keyed).first;

                        // Add to shared face cache if not found.
                        fontserver::font_face_set::iterator face_itr = std::find(face_set->begin(), face_set->end(), glyph.face);
                        if (face_itr == face_set->end()) {
                            face_set->add(glyph.face);
                        }
                    }

                    fontserver::tile_face *face = face_map_itr->second;

                    // Find out whether this font has been used in 
                    // this tile before and get its position.
                    std::vector<fontserver::tile_face *>::iterator tile_itr = std::find(tile_faces.begin(), tile_faces.end(), face);
                    if (tile_itr == tile_faces.end()) {
                        tile_faces.push_back(face);
                        tile_itr = tile_faces.end() - 1;
                    }

                    int tile_face_id = tile_itr - tile_faces.begin();

                    // Add glyph to tile_face.
                    face->add_glyph(glyph);

                    label->add_faces(tile_face_id);
                    label->add_glyphs(glyph.glyph_index);
                    label->add_x(width_map_[glyph.char_index] + glyph.offset.x);
                    label->add_y(glyph.ascender + glyph.offset.y);
                }

                itemizer.clear();
            }
        }

        // Add a textual representation of the font so that we can figure out
        // later what font we need to use.
        for (auto const& face : tile_faces) {
            std::string name = face->family + " " + face->style;
            mutable_layer->add_faces(name);
            // We don't delete the TileFace objects here because
            // they are 'owned' by the global faces map and deleted
            // later on.
        }

        // Insert FAKE stacks
        mutable_layer->add_stacks(baton->fontstack);
    }

    // Insert SDF glyphs + bitmaps
    for (auto const& face : tile_faces) {
        llmr::vector::face *mutable_face = tile.add_faces();
        mutable_face->set_family(face->family);
        mutable_face->set_style(face->style);

        for (auto const& glyph : face->glyphs) {
            llmr::vector::glyph *mutable_glyph = mutable_face->add_glyphs();
            mutable_glyph->set_id(glyph.glyph_index);
            mutable_glyph->set_width(glyph.width);
            mutable_glyph->set_height(glyph.height);
            mutable_glyph->set_left(glyph.left);
            mutable_glyph->set_top(glyph.top);
            mutable_glyph->set_advance(glyph.advance);
            if (glyph.width > 0) {
                mutable_glyph->set_bitmap(glyph.bitmap);
            }
        }
    }
}

void Glyphs::ShapeAfter(uv_work_t* req) {
    v8::HandleScope scope;
    ShapeBaton* baton = static_cast<ShapeBaton*>(req->data);

    const unsigned argc = 1;
    v8::Local<v8::Value> argv[argc] = { v8::Local<v8::Value>::New(v8::Null()) };

    v8::TryCatch try_catch;
    baton->callback->Call(v8::Context::GetCurrent()->Global(), argc, argv);
    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }

    baton->callback.Dispose();

    delete baton;
    delete req;
}
