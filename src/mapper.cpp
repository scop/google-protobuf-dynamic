#include "mapper.h"
#include "dynamic.h"

#undef New

#include <upb/pb/encoder.h>
#include <upb/pb/decoder.h>

using namespace gpd;
using namespace std;
using namespace upb;
using namespace upb::pb;

#ifdef MULTIPLICITY
#    define THX_DECLARE_AND_GET tTHX my_perl = cxt->my_perl
#else
#    define THX_DECLARE_AND_GET
#endif

namespace {
    void unref_on_scope_leave(void *ref) {
        ((Refcounted *) ref)->unref();
    }

    void refcounted_mortalize(pTHX_ const Refcounted *ref) {
        SAVEDESTRUCTOR(unref_on_scope_leave, ref);
    }
}

Mapper::DecoderHandlers::DecoderHandlers(pTHX_ const Mapper *mapper) {
    SET_THX_MEMBER;
    mappers.push_back(mapper);
}

void Mapper::DecoderHandlers::prepare(HV *target) {
    mappers.resize(1);
    seen_fields.resize(1);
    seen_fields.back().clear();
    seen_fields.back().resize(mappers.back()->fields.size());
    if (int oneof_count = mappers.back()->message_def->oneof_count()) {
        seen_oneof.resize(1);
        seen_oneof.back().clear();
        seen_oneof.back().resize(oneof_count, -1);
    }
    items.resize(1);
    error.clear();
    items[0] = (SV *) target;
    string = NULL;
}

SV *Mapper::DecoderHandlers::get_target() {
    return items[0];
}

void Mapper::DecoderHandlers::clear() {
    SvREFCNT_dec(items[0]);
}

namespace {
    inline void set_bool(pTHX_ SV *target, bool value) {
        if (value)
            sv_setiv(target, 1);
        else
            sv_setpvn(target, "", 0);
    }
}

string Mapper::Field::full_name() const {
    if (field_def->is_extension())
        return field_def->full_name();
    else
        return string(field_def->containing_type()->full_name()) +
               '.' +
               field_def->name();
}

bool Mapper::DecoderHandlers::apply_defaults_and_check() {
    const vector<bool> &seen = seen_fields.back();
    const Mapper *mapper = mappers.back();
    const vector<Mapper::Field> &fields = mapper->fields;
    bool decode_explict_defaults = mapper->decode_explicit_defaults;
    bool check_required_fields = mapper->check_required_fields;

    for (int i = 0, n = fields.size(); i < n; ++i) {
        const Mapper::Field &field = fields[i];
        bool field_seen = seen[i];

        if (!field_seen && decode_explict_defaults && field.has_default) {
            SV *target = get_target(&i);

            // this is the case where we merged multiple message instances,
            // so defaults have already been applied
            if (SvOK(target))
                continue;

            switch (field.field_def->type()) {
            case UPB_TYPE_FLOAT:
                sv_setnv(target, field.field_def->default_float());
                break;
            case UPB_TYPE_DOUBLE:
                sv_setnv(target, field.field_def->default_double());
                break;
            case UPB_TYPE_BOOL:
                set_bool(target, field.field_def->default_bool());
                break;
            case UPB_TYPE_BYTES:
            case UPB_TYPE_STRING: {
                size_t len;
                const char *val = field.field_def->default_string(&len);
                if (!val) {
                    val = "";
                    len = 0;
                }

                sv_setpvn(target, val, len);
                if (field.field_def->type() == UPB_TYPE_STRING)
                    SvUTF8_on(target);
            }
                break;
            case UPB_TYPE_ENUM:
            case UPB_TYPE_INT32:
                sv_setiv(target, field.field_def->default_int32());
                break;
            case UPB_TYPE_UINT32:
                sv_setuv(target, field.field_def->default_uint32());
                break;
            case UPB_TYPE_INT64:
                sv_setiv(target, field.field_def->default_int64());
                break;
            case UPB_TYPE_UINT64:
                sv_setuv(target, field.field_def->default_uint64());
                break;
            }
        } else if (!field_seen && check_required_fields && field.field_def->label() == UPB_LABEL_REQUIRED) {
            error = "Missing required field " + field.full_name();

            return false;
        }
    }

    return true;
}

bool Mapper::DecoderHandlers::on_end_message(DecoderHandlers *cxt, upb::Status *status) {
    if (!status || status->ok()) {
        return cxt->apply_defaults_and_check();
    } else
        return false;
}

Mapper::DecoderHandlers *Mapper::DecoderHandlers::on_start_string(DecoderHandlers *cxt, const int *field_index, size_t size_hint) {
    cxt->mark_seen(field_index);
    cxt->string = cxt->get_target(field_index);

    return cxt;
}

size_t Mapper::DecoderHandlers::on_string(DecoderHandlers *cxt, const int *field_index, const char *buf, size_t len) {
    THX_DECLARE_AND_GET;

    if (!SvOK(cxt->string))
        sv_setpvn(cxt->string, buf, len);
    else
        sv_catpvn(cxt->string, buf, len);

    return len;
}

bool Mapper::DecoderHandlers::on_end_string(DecoderHandlers *cxt, const int *field_index) {
    const Mapper *mapper = cxt->mappers.back();
    if (mapper->fields[*field_index].field_def->type() == UPB_TYPE_STRING)
        SvUTF8_on(cxt->string);
    cxt->string = NULL;

    return true;
}

Mapper::DecoderHandlers *Mapper::DecoderHandlers::on_start_sequence(DecoderHandlers *cxt, const int *field_index) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    SV *target = cxt->get_target(field_index);
    AV *av = NULL;

    if (!SvROK(target)) {
        av = newAV();

        SvUPGRADE(target, SVt_RV);
        SvROK_on(target);
        SvRV_set(target, (SV *) av);
    } else
        av = (AV *) SvRV(target);

    cxt->items.push_back((SV *) av);

    return cxt;
}

bool Mapper::DecoderHandlers::on_end_sequence(DecoderHandlers *cxt, const int *field_index) {
    cxt->items.pop_back();

    return true;
}

Mapper::DecoderHandlers *Mapper::DecoderHandlers::on_start_sub_message(DecoderHandlers *cxt, const int *field_index) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    const Mapper *mapper = cxt->mappers.back();
    SV *target = cxt->get_target(field_index);
    HV *hv = NULL;

    if (!SvROK(target)) {
        hv = newHV();

        SvUPGRADE(target, SVt_RV);
        SvROK_on(target);
        SvRV_set(target, (SV *) hv);
    } else
        hv = (HV *) SvRV(target);

    cxt->items.push_back((SV *) hv);
    cxt->mappers.push_back(mapper->fields[*field_index].mapper);
    cxt->seen_fields.resize(cxt->seen_fields.size() + 1);
    cxt->seen_fields.back().resize(cxt->mappers.back()->fields.size());
    if (int oneof_count = cxt->mappers.back()->message_def->oneof_count()) {
        cxt->seen_oneof.resize(cxt->seen_oneof.size() + 1);
        cxt->seen_oneof.back().resize(oneof_count, -1);
    }
    sv_bless(target, cxt->mappers.back()->stash);

    return cxt;
}

bool Mapper::DecoderHandlers::on_end_sub_message(DecoderHandlers *cxt, const int *field_index) {
    cxt->items.pop_back();
    cxt->mappers.pop_back();
    cxt->seen_fields.pop_back();
    if (cxt->mappers.back()->message_def->oneof_count())
        cxt->seen_oneof.pop_back();

    return true;
}

template<class T>
bool Mapper::DecoderHandlers::on_nv(DecoderHandlers *cxt, const int *field_index, T val) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    sv_setnv(cxt->get_target(field_index), val);

    return true;
}

template<class T>
bool Mapper::DecoderHandlers::on_iv(DecoderHandlers *cxt, const int *field_index, T val) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    sv_setiv(cxt->get_target(field_index), val);

    return true;
}

template<class T>
bool Mapper::DecoderHandlers::on_uv(DecoderHandlers *cxt, const int *field_index, T val) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    sv_setuv(cxt->get_target(field_index), val);

    return true;
}

bool Mapper::DecoderHandlers::on_enum(DecoderHandlers *cxt, const int *field_index, int32_t val) {
    THX_DECLARE_AND_GET;

    const Field &field = cxt->mappers.back()->fields[*field_index];
    if (field.enum_values.find(val) == field.enum_values.end()) {
        // this will use the default value later, it's intentional
        // mark_seen is not called
        if (SvTYPE(cxt->items.back()) == SVt_PVAV)
            sv_setiv(cxt->get_target(field_index), field.field_def->default_int32());
        return true;
    }

    cxt->mark_seen(field_index);
    sv_setiv(cxt->get_target(field_index), val);

    return true;
}

namespace {
    bool set_bigint(pTHX_ SV *target, uint64_t value, bool negative) {
        dSP;

        // this code is horribly slow; it could be made slightly faster,
        // but I doubt there is any point
        char buffer[19] = "-0x"; // -0x8000000000000000
        for (int i = 15; i >= 0; --i, value >>= 4) {
            int digit = value & 0xf;

            buffer[3 + i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }

        PUSHMARK(SP);
        XPUSHs(sv_2mortal(newSVpvs("Math::BigInt")));
        XPUSHs(sv_2mortal(newSVpvn(negative ? buffer : buffer + 1, negative ? 19 : 18)));
        PUTBACK;

        int count = call_method("new", G_SCALAR);

        SPAGAIN;
        SV *res = POPs;
        PUTBACK;

        sv_setsv(target, res);

        return true;
    }
}

bool Mapper::DecoderHandlers::on_bigiv(DecoderHandlers *cxt, const int *field_index, int64_t val) {
    cxt->mark_seen(field_index);

    if (val >= I32_MIN && val <= I32_MAX) {
        sv_setiv(cxt->get_target(field_index), (IV) val);

        return true;
    } else {
        THX_DECLARE_AND_GET;

        return set_bigint(aTHX_ cxt->get_target(field_index), (uint64_t) val, val < 0);
    }
}

bool Mapper::DecoderHandlers::on_biguv(DecoderHandlers *cxt, const int *field_index, uint64_t val) {
    cxt->mark_seen(field_index);

    if (val <= U32_MAX) {
        sv_setiv(cxt->get_target(field_index), (IV) val);

        return true;
    } else {
        THX_DECLARE_AND_GET;

        return set_bigint(aTHX_ cxt->get_target(field_index), val, false);
    }
}

bool Mapper::DecoderHandlers::on_bool(DecoderHandlers *cxt, const int *field_index, bool val) {
    THX_DECLARE_AND_GET;

    cxt->mark_seen(field_index);
    set_bool(cxt->get_target(field_index), val);

    return true;
}

SV *Mapper::DecoderHandlers::get_target(const int *field_index) {
    const Mapper *mapper = mappers.back();
    const Field &field = mapper->fields[*field_index];
    SV *curr = items.back();

    if (SvTYPE(curr) == SVt_PVAV) {
        AV *av = (AV *) curr;

        return *av_fetch(av, av_top_index(av) + 1, 1);
    } else {
        HV *hv = (HV *) curr;

        if (field.oneof_index != -1) {
            int32_t seen = seen_oneof.back()[field.oneof_index];

            if (seen != -1 && seen != *field_index) {
                const Field &field = mapper->fields[seen];

                hv_delete_ent(hv, field.name, G_DISCARD, field.name_hash);
            }
            seen_oneof.back()[field.oneof_index] = *field_index;
        }

        return HeVAL(hv_fetch_ent(hv, field.name, 1, field.name_hash));
    }
}

void Mapper::DecoderHandlers::mark_seen(const int *field_index) {
    seen_fields.back()[*field_index] = true;
}

Mapper::Mapper(pTHX_ Dynamic *_registry, const MessageDef *_message_def, HV *_stash, const MappingOptions &options) :
        registry(_registry),
        message_def(_message_def),
        stash(_stash),
        decoder_callbacks(aTHX_ this),
        string_sink(&output_buffer) {
    SET_THX_MEMBER;

    SvREFCNT_inc(stash);

    env.ReportErrorsTo(&status);
    registry->ref();
    encoder_handlers = Encoder::NewHandlers(message_def);
    decoder_handlers = Handlers::New(message_def);
    decode_explicit_defaults = options.explicit_defaults;
    encode_defaults = options.encode_defaults;
    check_enum_values = options.check_enum_values;

    if (!decoder_handlers->SetEndMessageHandler(UpbMakeHandler(DecoderHandlers::on_end_message)))
        croak("Unable to set upb end message handler for %s", message_def->full_name());

    bool has_required = false;
    for (MessageDef::const_field_iterator it = message_def->field_begin(), en = message_def->field_end(); it != en; ++it) {
        int index = fields.size();
        fields.push_back(Field());

        Field &field = fields.back();;
        const FieldDef *field_def = *it;
        const OneofDef *oneof_def = field_def->containing_oneof();

        has_required = has_required || field_def->label() == UPB_LABEL_REQUIRED;
        field.field_def = field_def;
        if (field_def->is_extension()) {
            string temp = string() + "[" + field_def->full_name() + "]";

            field.name = newSVpv_share(temp.c_str(), 0);
        } else {
            field.name = newSVpv_share(field_def->name(), 0);
        }
        field.name_hash = SvSHARED_HASH(field.name);
        field.mapper = NULL;
        field.oneof_index = -1;

#define GET_SELECTOR(KIND, TO) \
    ok = ok && encoder_handlers->GetSelector(field_def, UPB_HANDLER_##KIND, &field.selector.TO)

#define SET_VALUE_HANDLER(TYPE, FUNCTION) \
    ok = ok && decoder_handlers->SetValueHandler<TYPE>(field_def, UpbBind(DecoderHandlers::FUNCTION, new int(index)))

#define SET_HANDLER(KIND, FUNCTION) \
    ok = ok && decoder_handlers->Set##KIND##Handler(field_def, UpbBind(DecoderHandlers::FUNCTION, new int(index)))

        bool ok = true;
        bool has_default = true;
        switch (field_def->type()) {
        case UPB_TYPE_FLOAT:
            GET_SELECTOR(FLOAT, primitive);
            SET_VALUE_HANDLER(float, on_nv<float>);
            break;
        case UPB_TYPE_DOUBLE:
            GET_SELECTOR(DOUBLE, primitive);
            SET_VALUE_HANDLER(double, on_nv<double>);
            break;
        case UPB_TYPE_BOOL:
            GET_SELECTOR(BOOL, primitive);
            SET_VALUE_HANDLER(bool, on_bool);
            break;
        case UPB_TYPE_STRING:
        case UPB_TYPE_BYTES:
            GET_SELECTOR(STARTSTR, str_start);
            GET_SELECTOR(STRING, str_cont);
            GET_SELECTOR(ENDSTR, str_end);
            SET_HANDLER(StartString, on_start_string);
            SET_HANDLER(String, on_string);
            SET_HANDLER(EndString, on_end_string);
            break;
        case UPB_TYPE_MESSAGE:
            GET_SELECTOR(STARTSUBMSG, msg_start);
            GET_SELECTOR(ENDSUBMSG, msg_end);
            SET_HANDLER(StartSubMessage, on_start_sub_message);
            SET_HANDLER(EndSubMessage, on_end_sub_message);
            has_default = false;
            break;
        case UPB_TYPE_ENUM: {
            GET_SELECTOR(INT32, primitive);
            if (check_enum_values)
                SET_VALUE_HANDLER(int32_t, on_enum);
            else
                SET_VALUE_HANDLER(int32_t, on_iv<int32_t>);

            if (check_enum_values) {
                const EnumDef *enumdef = field_def->enum_subdef();
                upb_enum_iter i;
                for (upb_enum_begin(&i, enumdef); !upb_enum_done(&i); upb_enum_next(&i))
                    field.enum_values.insert(upb_enum_iter_number(&i));
            }
        }
            break;
        case UPB_TYPE_INT32:
            GET_SELECTOR(INT32, primitive);
            SET_VALUE_HANDLER(int32_t, on_iv<int32_t>);
            break;
        case UPB_TYPE_UINT32:
            GET_SELECTOR(UINT32, primitive);
            SET_VALUE_HANDLER(uint32_t, on_uv<uint32_t>);
            break;
        case UPB_TYPE_INT64:
            GET_SELECTOR(INT64, primitive);
            if (options.use_bigints)
                SET_VALUE_HANDLER(int64_t, on_bigiv);
            else
                SET_VALUE_HANDLER(int64_t, on_iv<int64_t>);
            break;
        case UPB_TYPE_UINT64:
            GET_SELECTOR(UINT64, primitive);
            if (options.use_bigints)
                SET_VALUE_HANDLER(uint64_t, on_biguv);
            else
                SET_VALUE_HANDLER(uint64_t, on_uv<uint64_t>);
            break;
        default:
            croak("Unhandled field type %d for field '%s'", field_def->type(), field.full_name().c_str());
        }

        if (has_default &&
                field_def->label() == UPB_LABEL_OPTIONAL &&
                !oneof_def) {
            field.has_default = true;
        }

        if (field_def->label() == UPB_LABEL_REPEATED) {
            GET_SELECTOR(STARTSEQ, seq_start);
            GET_SELECTOR(ENDSEQ, seq_end);
            SET_HANDLER(StartSequence, on_start_sequence);
            SET_HANDLER(EndSequence, on_end_sequence);
        }

#undef GET_SELECTOR
#undef SET_VALUE_HANDLER
#undef SET_HANDLER

        if (!ok)
            croak("Unable to get upb selector for field %s", field.full_name().c_str());
    }

    int oneof_index = 0;
    for (MessageDef::const_oneof_iterator it = message_def->oneof_begin(), en = message_def->oneof_end(); it != en; ++it, ++oneof_index) {
        const OneofDef *oneof_def = *it;

        for (OneofDef::const_iterator it = oneof_def->begin(), en = oneof_def->end(); it != en; ++it) {
            const FieldDef *field_def = *it;
            Field &field = fields[field_def->index()];

            field.oneof_index = oneof_index;
        }
    }

    check_required_fields = has_required && options.check_required_fields;
}

Mapper::~Mapper() {
    for (vector<Field>::iterator it = fields.begin(), en = fields.end(); it != en; ++it)
        if (it->mapper)
            it->mapper->unref();
    // make sure this only goes away after inner destructors have completed
    refcounted_mortalize(registry);
    SvREFCNT_dec(stash);
}

int Mapper::field_count() const {
    return fields.size();
}

const Mapper::Field *Mapper::get_field(int index) const {
    return &fields[index];
}

void Mapper::resolve_mappers() {
    for (vector<Field>::iterator it = fields.begin(), en = fields.end(); it != en; ++it) {
        const FieldDef *field = it->field_def;

        if (field->type() != UPB_TYPE_MESSAGE)
            continue;
        it->mapper = registry->find_mapper(field->message_subdef());
        it->mapper->ref();
        decoder_handlers->SetSubHandlers(it->field_def, it->mapper->decoder_handlers.get());
    }

    decoder_method = DecoderMethod::New(DecoderMethodOptions(decoder_handlers.get()));
    decoder_sink.Reset(decoder_handlers.get(), &decoder_callbacks);
    decoder = upb::pb::Decoder::Create(&env, decoder_method.get(), &decoder_sink);
    encoder = upb::pb::Encoder::Create(&env, encoder_handlers.get(), string_sink.input());
}

SV *Mapper::encode_from_perl(SV *ref) {
    status.Clear();
    output_buffer.clear();
    SV *result = NULL;
    if (encode_from_perl(encoder, encoder->input(), &status, ref))
        result = newSVpvn(output_buffer.data(), output_buffer.size());
    output_buffer.clear();

    return result;
}

SV *Mapper::decode_to_perl(const char *buffer, STRLEN bufsize) {
    status.Clear();
    decoder->Reset();
    decoder_callbacks.prepare(newHV());

    SV *result = NULL;
    if (BufferSource::PutBuffer(buffer, bufsize, decoder->input()))
        result = sv_bless(newRV_inc(decoder_callbacks.get_target()), stash);
    decoder_callbacks.clear();

    return result;
}

const char *Mapper::last_error_message() const {
    return !decoder_callbacks.error.empty() ? decoder_callbacks.error.c_str() :
           !status.ok()                     ? status.error_message() :
                                              "Unknown error";
}

namespace {
    // this code is horribly slow; it could be made slightly faster,
    // but I doubt there is any point
    uint64_t extract_bits(pTHX_ SV *src, bool *negative) {
        dSP;

        PUSHMARK(SP);
        XPUSHs(src);
        PUTBACK;

        int count = call_method("as_hex", G_SCALAR);

        SPAGAIN;
        SV *res = POPs;
        PUTBACK;

        uint64_t integer = 0;
        const char *buffer = SvPV_nolen(res);
        *negative = buffer[0] == '-';

        if (*negative)
            ++buffer;
        buffer += 2; // skip 0x
        while (*buffer) {
            integer <<= 4;
            integer |= READ_XDIGIT(buffer);
        }

        return integer;
    }

    uint64_t get_uint64(pTHX_ SV *src) {
        if (SvROK(src) && sv_derived_from(src, "Math::BigInt")) {
            bool negative = false;
            uint64_t value = extract_bits(aTHX_ src, &negative);

            return negative ? ~value + 1 : value;
        } else
            return SvUV(src);
    }

    int64_t get_int64(pTHX_ SV *src) {
        if (SvROK(src) && sv_derived_from(src, "Math::BigInt")) {
            bool negative = false;
            uint64_t value = extract_bits(aTHX_ src, &negative);

            return negative ? -value : value;
        } else
            return SvIV(src);
    }

    class SVGetter {
    public:
        SV *operator()(pTHX_ SV *src) const { return src; }
    };

    class NVGetter {
    public:
        NV operator()(pTHX_ SV *src) const { return SvNV(src); }
    };

    class IVGetter {
    public:
        IV operator()(pTHX_ SV *src) const { return SvIV(src); }
    };

    class UVGetter {
    public:
        UV operator()(pTHX_ SV *src) const { return SvUV(src); }
    };

    class I64Getter {
    public:
        int64_t operator()(pTHX_ SV *src) const { return get_int64(aTHX_ src); }
    };

    class U64Getter {
    public:
        uint64_t operator()(pTHX_ SV *src) const { return get_uint64(aTHX_ src); }
    };

    class StringGetter {
    public:
        void operator()(pTHX_ SV *src, string *dest) const {
            STRLEN len;
            const char *buf = SvPVutf8(src, len);

            dest->assign(buf, len);
        }
    };

    class BytesGetter {
    public:
        void operator()(pTHX_ SV *src, string *dest) const {
            STRLEN len;
            const char *buf = SvPV(src, len);

            dest->assign(buf, len);
        }
    };

    class BoolGetter {
    public:
        bool operator()(pTHX_ SV *src) const { return SvTRUE(src); }
    };

#define DEF_SIMPLE_SETTER(NAME, METHOD, TYPE)   \
    struct NAME { \
        NAME(Status *status) { } \
        bool operator()(pTHX_ Sink *sink, const Mapper::Field &fd, TYPE value) { \
            return sink->METHOD(fd.selector.primitive, value);    \
        } \
    }

    DEF_SIMPLE_SETTER(Int32Emitter, PutInt32, int32_t);
    DEF_SIMPLE_SETTER(Int64Emitter, PutInt64, int64_t);
    DEF_SIMPLE_SETTER(UInt32Emitter, PutUInt32, uint32_t);
    DEF_SIMPLE_SETTER(UInt64Emitter, PutUInt64, uint64_t);
    DEF_SIMPLE_SETTER(FloatEmitter, PutFloat, float);
    DEF_SIMPLE_SETTER(DoubleEmitter, PutDouble, double);
    DEF_SIMPLE_SETTER(BoolEmitter, PutBool, bool);

#undef DEF_SIMPLE_SETTER

    struct EnumEmitter {
        Status *status;

        EnumEmitter(Status *_status) { status = _status; }

        bool operator()(pTHX_ Sink *sink, const Mapper::Field &fd, int32_t value) {
            if (fd.enum_values.find(value) == fd.enum_values.end()) {
                status->SetFormattedErrorMessage(
                    "Invalid enumeration value %d for field '%s'",
                    value,
                    fd.full_name().c_str()
                );
                return false;
            }

            return sink->PutInt32(fd.selector.primitive, value);
        }
    };

    struct StringEmitter {
        StringEmitter(Status *status) { }

        bool operator()(pTHX_ Sink *sink, const Mapper::Field &fd, SV *value) {
            STRLEN len;
            const char *str = fd.field_def->type() == UPB_TYPE_STRING ? SvPVutf8(value, len) : SvPV(value, len);
            Sink sub;
            if (!sink->StartString(fd.selector.str_start, len, &sub))
                return false;
            sub.PutStringBuffer(fd.selector.str_cont, str, len, NULL);
            return sink->EndString(fd.selector.str_end);
        }
    };
}

template<class G, class S>
bool Mapper::encode_from_array(Encoder *encoder, Sink *sink, Status *status, const Mapper::Field &fd, AV *source) const {
    G getter;
    S setter(status);
    Sink sub, *actual = sink;
    bool packed = upb_fielddef_isprimitive(fd.field_def) && fd.field_def->packed();

    if (packed) {
        if (!sink->StartSequence(fd.selector.seq_start, &sub))
            return false;
        actual = &sub;
    }
    int size = av_top_index(source) + 1;

    for (int i = 0; i < size; ++i) {
        SV **item = av_fetch(source, i, 0);
        if (!item)
            return false;

        if (!setter(aTHX_ actual, fd, getter(aTHX_ *item)))
            return false;
    }

    return !packed || sink->EndSequence(fd.selector.seq_end);
}

bool Mapper::encode_from_message_array(Encoder *encoder, Sink *sink, Status *status, const Mapper::Field &fd, AV *source) const {
    int size = av_top_index(source) + 1;

    for (int i = 0; i < size; ++i) {
        SV **item = av_fetch(source, i, 0);
        if (!item)
            return false;
        Sink submsg;

        if (!sink->StartSubMessage(fd.selector.msg_start, &submsg))
            return false;
        if (!encode_from_perl(encoder, &submsg, status, *item))
            return false;
        if (!sink->EndSubMessage(fd.selector.msg_end))
            return false;
    }

    return true;
}

bool Mapper::encode_from_perl(Encoder* encoder, Sink *sink, Status *status, SV *ref) const {
    if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVHV)
        croak("Not an hash reference when encoding a %s value", message_def->full_name());
    HV *hv = (HV *) SvRV(ref);

    if (!sink->StartMessage())
        return false;

    bool ok = true;
    int last_oneof = -1;
    for (vector<Field>::const_iterator it = fields.begin(), en = fields.end(); it != en; ++it) {
        HE *he = hv_fetch_ent(hv, it->name, 0, it->name_hash);

        if (!he) {
            if (it->field_def->label() == UPB_LABEL_REQUIRED) {
                status->SetFormattedErrorMessage(
                    "Missing required field '%s'",
                    it->full_name().c_str());
                return false;
            } else
                continue;
        } else if (it->oneof_index != -1) {
            if (it->oneof_index == last_oneof)
                continue;
            else
                last_oneof = it->oneof_index;
        }

        if (it->field_def->label() == UPB_LABEL_REPEATED)
            ok = ok && encode_from_perl_array(encoder, sink, status, *it, HeVAL(he));
        else
            ok = ok && encode_from_perl(encoder, sink, status, *it, HeVAL(he));
    }

    if (!sink->EndMessage(status))
        return false;

    return ok;
}

bool Mapper::encode_from_perl(Encoder* encoder, Sink *sink, Status *status, const Field &fd, SV *ref) const {
    switch (fd.field_def->type()) {
    case UPB_TYPE_FLOAT:
        return sink->PutFloat(fd.selector.primitive, SvNV(ref));
    case UPB_TYPE_DOUBLE:
        return sink->PutDouble(fd.selector.primitive, SvNV(ref));
    case UPB_TYPE_BOOL:
        return sink->PutBool(fd.selector.primitive, SvTRUE(ref));
    case UPB_TYPE_STRING:
    case UPB_TYPE_BYTES: {
        STRLEN len;
        const char *str = fd.field_def->type() == UPB_TYPE_STRING ? SvPVutf8(ref, len) : SvPV(ref, len);
        Sink sub;
        if (!sink->StartString(fd.selector.str_start, len, &sub))
            return false;
        sub.PutStringBuffer(fd.selector.str_cont, str, len, NULL);
        return sink->EndString(fd.selector.str_end);
    }
    case UPB_TYPE_MESSAGE: {
        Sink sub;
        if (!sink->StartSubMessage(fd.selector.msg_start, &sub))
            return false;
        if (!fd.mapper->encode_from_perl(encoder, &sub, status, ref))
            return false;
        return sink->EndSubMessage(fd.selector.msg_end);
    }
    case UPB_TYPE_ENUM: {
        IV value = SvIV(ref);
        if (check_enum_values &&
                fd.enum_values.find(value) == fd.enum_values.end()) {
            status->SetFormattedErrorMessage(
                "Invalid enumeration value %d for field '%s'",
                value,
                fd.full_name().c_str()
            );
            return false;
        }

        return sink->PutInt32(fd.selector.primitive, value);
    }
    case UPB_TYPE_INT32:
        return sink->PutInt32(fd.selector.primitive, SvIV(ref));
    case UPB_TYPE_UINT32:
        return sink->PutUInt32(fd.selector.primitive, SvUV(ref));
    case UPB_TYPE_INT64:
        if (sizeof(IV) >= sizeof(int64_t))
            return sink->PutInt64(fd.selector.primitive, SvIV(ref));
        else
            return sink->PutInt64(fd.selector.primitive, get_int64(aTHX_ ref));
    case UPB_TYPE_UINT64:
        if (sizeof(UV) >= sizeof(int64_t))
            return sink->PutInt64(fd.selector.primitive, SvUV(ref));
        else
            return sink->PutUInt64(fd.selector.primitive, get_uint64(aTHX_ ref));
    default:
        return false; // just in case
    }
}

bool Mapper::encode_from_perl_array(Encoder* encoder, Sink *sink, Status *status, const Field &fd, SV *ref) const {
    if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVAV)
        croak("Not an array reference when encoding field '%s'", fd.full_name().c_str());
    AV *array = (AV *) SvRV(ref);

    switch (fd.field_def->type()) {
    case UPB_TYPE_FLOAT:
        return encode_from_array<NVGetter, FloatEmitter>(encoder, sink, fd, array);
    case UPB_TYPE_DOUBLE:
        return encode_from_array<NVGetter, DoubleEmitter>(encoder, sink, fd, array);
    case UPB_TYPE_BOOL:
        return encode_from_array<BoolGetter, BoolEmitter>(encoder, sink, fd, array);
    case UPB_TYPE_STRING:
        return encode_from_array<SVGetter, StringEmitter>(encoder, sink, fd, array);
    case UPB_TYPE_BYTES:
        return encode_from_array<SVGetter, StringEmitter>(encoder, sink, fd, array);
    case UPB_TYPE_MESSAGE:
        return fd.mapper->encode_from_message_array(encoder, sink, status, fd, array);
    case UPB_TYPE_ENUM:
        if (check_enum_values)
            return encode_from_array<IVGetter, EnumEmitter>(encoder, sink, status, fd, array);
        else
            return encode_from_array<IVGetter, Int32Emitter>(encoder, sink, fd, array);
    case UPB_TYPE_INT32:
        return encode_from_array<IVGetter, Int32Emitter>(encoder, sink, fd, array);
    case UPB_TYPE_UINT32:
        return encode_from_array<UVGetter, UInt32Emitter>(encoder, sink, fd, array);
    case UPB_TYPE_INT64:
        if (sizeof(IV) >= sizeof(int64_t))
            return encode_from_array<IVGetter, Int64Emitter>(encoder, sink, fd, array);
        else
            return encode_from_array<I64Getter, Int64Emitter>(encoder, sink, fd, array);
    case UPB_TYPE_UINT64:
        if (sizeof(IV) >= sizeof(int64_t))
            return encode_from_array<UVGetter, UInt64Emitter>(encoder, sink, fd, array);
        else
            return encode_from_array<U64Getter, UInt64Emitter>(encoder, sink, fd, array);
    default:
        return false; // just in case
    }
}

MapperField::MapperField(const Mapper *_mapper, const Mapper::Field *_field) :
        field(_field),
        mapper(_mapper) {
    mapper->ref();
}

MapperField::~MapperField() {
    mapper->unref();
}

SV *MapperField::get_read_field(HV *self) {
    HE *ent = hv_fetch_ent(self, field->name, 0, field->name_hash);

    return ent ? HeVAL(ent) : NULL;
}

SV *MapperField::get_write_field(HV *self) {
    HE *ent = hv_fetch_ent(self, field->name, 1, field->name_hash);

    return HeVAL(ent);
}

AV *MapperField::get_read_array(HV *self) {
    HE *ent = hv_fetch_ent(self, field->name, 0, field->name_hash);

    if (!ent)
        return NULL;

    SV *ref = HeVAL(ent);
    if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVAV)
        croak("Value of field '%s' is not an array reference", field->full_name().c_str());

    return (AV *) SvRV(ref);
}

AV *MapperField::get_write_array(HV *self) {
    HE *ent = hv_fetch_ent(self, field->name, 1, field->name_hash);
    SV *ref = HeVAL(ent);

    if (!SvOK(ref)) {
        AV *av = newAV();

        SvUPGRADE(ref, SVt_RV);
        SvROK_on(ref);
        SvRV_set(ref, (SV *) av);

        return av;
    } else {
        SV *ref = HeVAL(ent);
        if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVAV)
            croak("Value of field '%s' is not an array reference", field->full_name().c_str());

        return (AV *) SvRV(ref);
    }
}

const char *MapperField::name() {
    return field->field_def->name();
}

bool MapperField::is_repeated() {
    return field->field_def->label() == UPB_LABEL_REPEATED;
}

bool MapperField::is_extension() {
    return field->field_def->is_extension();
}

bool MapperField::has_field(HV *self) {
    return hv_fetch_ent(self, field->name, 0, field->name_hash);
}

void MapperField::clear_field(HV *self) {
    hv_delete_ent(self, field->name, G_DISCARD, field->name_hash);
}

void MapperField::get_scalar(HV *self, SV *target) {
    SV *value = get_read_field(self);

    copy_value_or_default(target, value);
}

void MapperField::copy_value_or_default(SV *target, SV *value) {
    const FieldDef *field_def = field->field_def;

    switch (field_def->type()) {
    case UPB_TYPE_FLOAT:
        sv_setnv(target, value ? SvNV(value) : field_def->default_float());
        break;
    case UPB_TYPE_DOUBLE:
        sv_setnv(target, value ? SvNV(value) : field_def->default_double());
        break;
    case UPB_TYPE_BOOL:
        set_bool(target, value ? SvTRUE(value) : field_def->default_bool());
        break;
    case UPB_TYPE_STRING: {
        if (value) {
            STRLEN len;
            const char *str = SvPVutf8(value, len);

            sv_setpvn(target, str, len);
        } else {
            size_t len;
            const char *str = field_def->default_string(&len);

            sv_setpvn(target, str, len);
        }
        SvUTF8_on(target);
    }
        break;
    case UPB_TYPE_BYTES: {
        if (value) {
            STRLEN len;
            const char *str = SvPV(value, len);

            sv_setpvn(target, str, len);
        } else {
            size_t len;
            const char *str = field_def->default_string(&len);

            sv_setpvn(target, str, len);
        }
    }
        break;
    case UPB_TYPE_MESSAGE:
        if ((value && SvOK(value)) && (!SvROK(value) || SvTYPE(SvRV(value)) != SVt_PVHV))
            croak("Value for message field '%s' is not an hash reference", field->full_name().c_str());
        sv_setsv(target, value ? value : &PL_sv_undef);
        break;
    case UPB_TYPE_ENUM: {
        if (value) {
            IV i32 = SvIV(value);
            if (field->enum_values.size() &&
                    field->enum_values.find(i32) == field->enum_values.end())
                croak("Invalid value %d for enumeration field '%s'", i32, field->full_name().c_str());
            sv_setiv(target, i32);
        } else
            sv_setiv(target, field_def->default_int32());
    }
        break;
    case UPB_TYPE_INT32:
        sv_setiv(target, value ? SvIV(value) : field_def->default_int32());
        break;
    case UPB_TYPE_UINT32:
        sv_setuv(target, value ? SvUV(value) : field_def->default_uint32());
        break;
    case UPB_TYPE_INT64: {
        if (sizeof(IV) >= sizeof(int64_t))
            sv_setiv(target, value ? SvIV(value) : field_def->default_int64());
        else {
            int64_t i64 = value ? get_int64(value) : field_def->default_int64();

            set_bigint(aTHX_ target, (uint64_t) i64, i64 < 0);
        }
    }
        break;
    case UPB_TYPE_UINT64: {
        if (sizeof(IV) >= sizeof(uint64_t))
            sv_setuv(target, value ? SvUV(value) : field_def->default_uint64());
        else {
            int64_t u64 = value ? get_uint64(value) : field_def->default_uint64();

            set_bigint(aTHX_ target, u64, false);
        }
    }
        break;
    default:
        croak("Unhandled field type %d for field '%s'", field->field_def->type(), field->full_name().c_str());
    }
}

void MapperField::clear_oneof(HV *self) {
    for (int i = 0, max = mapper->field_count(); i < max; ++i) {
        const Mapper::Field *other = mapper->get_field(i);

        if (other == field)
            continue;
        hv_delete_ent(self, other->name, G_DISCARD, other->name_hash);
    }
}

void MapperField::set_scalar(HV *self, SV *value) {
    if (field->oneof_index != -1)
        clear_oneof(self);

    SV *target = get_write_field(self);

    copy_value(target, value);
}

void MapperField::copy_value(SV *target, SV *value) {
    const FieldDef *field_def = field->field_def;

    switch (field_def->type()) {
    case UPB_TYPE_FLOAT:
        sv_setnv(target, SvNV(value));
        break;
    case UPB_TYPE_DOUBLE:
        sv_setnv(target, SvNV(value));
        break;
    case UPB_TYPE_BOOL:
        set_bool(target, SvTRUE(value));
        break;
    case UPB_TYPE_STRING: {
        STRLEN len;
        const char *str = SvPVutf8(value, len);

        sv_setpvn(target, str, len);
        SvUTF8_on(target);
    }
        break;
    case UPB_TYPE_BYTES: {
        STRLEN len;
        const char *str = SvPV(value, len);

        sv_setpvn(target, str, len);
    }
        break;
    case UPB_TYPE_MESSAGE:
        if (SvOK(value) && (!SvROK(value) || SvTYPE(SvRV(value)) != SVt_PVHV))
            croak("Value for message field '%s' is not an hash reference", field->full_name().c_str());
        sv_setsv(target, value);
        break;
    case UPB_TYPE_ENUM: {
        I32 i32 = SvIV(value);
        if (field->enum_values.size() &&
                field->enum_values.find(i32) == field->enum_values.end())
            croak("Invalid value %d for enumeration field '%s'", i32, field->full_name().c_str());
        sv_setiv(target, i32);
    }
        break;
    case UPB_TYPE_INT32:
        sv_setiv(target, SvIV(value));
        break;
    case UPB_TYPE_UINT32:
        sv_setuv(target, SvUV(value));
        break;
    case UPB_TYPE_INT64: {
        if (sizeof(IV) >= sizeof(int64_t))
            sv_setiv(target, SvIV(value));
        else {
            int64_t i64 = get_int64(value);

            set_bigint(aTHX_ target, (uint64_t) i64, i64 < 0);
        }
    }
        break;
    case UPB_TYPE_UINT64: {
        if (sizeof(IV) >= sizeof(uint64_t))
            sv_setuv(target, SvUV(value));
        else {
            int64_t u64 = get_uint64(value);

            set_bigint(aTHX_ target, u64, false);
        }
    }
        break;
    default:
        croak("Unhandled field type %d for field '%s'", field->field_def->type(), field->full_name().c_str());
    }
}

void MapperField::get_item(HV *self, int index, SV *target) {
    AV *array = get_read_array(self);

    if (!array)
        croak("Accessing unset array field '%s'", field->full_name().c_str());
    int max = av_top_index(array);
    if (max == -1)
        croak("Accessing empty array field '%s'", field->full_name().c_str());
    if (index > max || index < -(max + 1))
        croak("Accessing out-of-bounds index %d for field '%s'", index, field->full_name().c_str());
    SV **value = av_fetch(array, index, 0);

    copy_value_or_default(target, value ? *value : NULL);
}

void MapperField::set_item(HV *self, int index, SV *value) {
    AV *array = get_write_array(self);
    SV **target = av_fetch(array, index, 1);

    copy_value(*target, value ? value : NULL);
}

void MapperField::add_item(HV *self, SV *value) {
    AV *array = get_write_array(self);
    SV **target = av_fetch(array, av_top_index(array) + 1, 1);

    copy_value(*target, value ? value : NULL);
}

int MapperField::list_size(HV *self) {
    AV *target = get_read_array(self);

    if (!target)
        return 0;

    return av_top_index(target) + 1;
}

void MapperField::get_list(HV *self, SV *target) {
    AV *array = get_read_array(self);

    if (!target) {
        sv_setsv(target, &PL_sv_undef);
    } else {
        SvUPGRADE(target, SVt_RV);
        SvROK_on(target);
        SvRV_set(target, (SV *) array);
        SvREFCNT_inc(array);
    }
}

void MapperField::set_list(HV *self, SV *ref) {
    if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVAV)
        croak("Value for field '%s' is not an array reference", field->full_name().c_str());
    SV *field_ref = get_write_field(self);

    if (!SvOK(field_ref)) {
        SvUPGRADE(field_ref, SVt_RV);
        SvROK_on(field_ref);
    } else {
        if (!SvROK(field_ref))
            croak("Value of field '%s' is not a reference", field->full_name().c_str());
        SvREFCNT_dec(SvRV(field_ref));
    }
    SvRV_set(field_ref, SvRV(ref));
    SvREFCNT_inc(SvRV(field_ref));
}
