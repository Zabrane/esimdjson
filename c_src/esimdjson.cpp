#include "erl_nif.h"
#include "simdjson.h"

/*TODO:
 * - On-demand parser
 */

ERL_NIF_TERM mk_atom(ErlNifEnv *env, const char *atom);
ERL_NIF_TERM mk_ok_result(ErlNifEnv *env, ERL_NIF_TERM result);
ERL_NIF_TERM mk_error(ErlNifEnv *env, const char *mesg);
static ERL_NIF_TERM nif_parse(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM nif_new(ErlNifEnv *env, int argc,
                            const ERL_NIF_TERM argv[]);
int load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info);
int make_term_from_dom(ErlNifEnv *env, simdjson::dom::element element,
                       ERL_NIF_TERM *term);
void dom_parser_dtor(ErlNifEnv *env, void *obj);

int load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info) {
  ErlNifResourceFlags flags =
      ErlNifResourceFlags(ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER);
  ErlNifResourceType *res_type = enif_open_resource_type(
      env, 0, "esimdjson_dom_parser", dom_parser_dtor, flags, 0);
  if (!res_type)
    return -1;
  *priv_data = (void *)res_type;
  return 0;
}

ERL_NIF_TERM mk_atom(ErlNifEnv *env, const char *atom) {
  ERL_NIF_TERM ret;

  if (!enif_make_existing_atom(env, atom, &ret, ERL_NIF_LATIN1)) {
    return enif_make_atom(env, atom);
  }

  return ret;
}

ERL_NIF_TERM mk_ok_result(ErlNifEnv *env, ERL_NIF_TERM result) {
  return enif_make_tuple2(env, mk_atom(env, "ok"), result);
}

ERL_NIF_TERM mk_error(ErlNifEnv *env, const char *mesg) {
  return enif_make_tuple2(env, mk_atom(env, "error"), mk_atom(env, mesg));
}

static ERL_NIF_TERM nif_new(ErlNifEnv *env, int argc,
                            const ERL_NIF_TERM argv[]) {
  /*TODO:
   * - max_capacity option
   * (https://github.com/simdjson/simdjson/blob/master/doc/performance.md#reusing-the-parser-for-maximum-efficiency)
   * - fixed_capacity option (ditto)
   */
  ErlNifResourceType *res_type = (ErlNifResourceType *)enif_priv_data(env);

  void *parser_res =
      enif_alloc_resource(res_type, sizeof(simdjson::dom::parser));

  // "placement new"
  new (parser_res) simdjson::dom::parser();

  ERL_NIF_TERM res_term = enif_make_resource(env, parser_res);
  enif_release_resource(parser_res);

  return mk_ok_result(env, res_term);
}

static ERL_NIF_TERM nif_parse(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  if (argc != 2)
    return enif_make_badarg(env);

  ErlNifResourceType *res_type = (ErlNifResourceType *)enif_priv_data(env);
  simdjson::dom::parser *pparser;
  if (!enif_get_resource(env, argv[0], res_type, (void **)&pparser))
    return enif_make_badarg(env);

  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[1], &bin))
    return enif_make_badarg(env);

  simdjson::dom::element element = pparser->parse((char *)bin.data, bin.size);

  ERL_NIF_TERM result;
  make_term_from_dom(env, element, &result);

  return mk_ok_result(env, result);
}

int make_term_from_dom(ErlNifEnv *env, simdjson::dom::element element,
                       ERL_NIF_TERM *term) {
  switch (element.type()) {
  case simdjson::dom::element_type::INT64:
    *term = enif_make_int64(env, int64_t(element));
    break;
  case simdjson::dom::element_type::UINT64:
    *term = enif_make_uint64(env, uint64_t(element));
    break;
  case simdjson::dom::element_type::DOUBLE:
    *term = enif_make_double(env, double(element));
    break;
  case simdjson::dom::element_type::STRING: {
    std::string_view str = std::string_view(element);
    char *str_bin = (char *)enif_make_new_binary(env, str.size(), term);
    str.copy(str_bin, str.size());
  } break;
  case simdjson::dom::element_type::OBJECT: {
    simdjson::dom::object obj = simdjson::dom::object(element);
    ERL_NIF_TERM keys[obj.size()];
    ERL_NIF_TERM values[obj.size()];

    size_t i = 0;
    for (auto [key, value] : obj) {
      ERL_NIF_TERM k;
      char *k_bin = (char *)enif_make_new_binary(env, key.size(), &k);
      key.copy(k_bin, key.size());

      ERL_NIF_TERM v;
      make_term_from_dom(env, value, &v);
      keys[i] = k;
      values[i] = v;
      i++;
    }
    enif_make_map_from_arrays(env, keys, values, obj.size(), term);

  } break;
  case simdjson::dom::element_type::ARRAY: {
    ERL_NIF_TERM cons = enif_make_list(env, 0);

    simdjson::dom::array array = (simdjson::dom::array)element;

    for (size_t i = array.size(); i > 0; i--) {
      simdjson::dom::element e = array.at(i - 1);
      ERL_NIF_TERM car;
      make_term_from_dom(env, e, &car);
      cons = enif_make_list_cell(env, car, cons);
    }
    *term = cons;

  } break;
  default:
    break;
  }

  return 0;
}

void dom_parser_dtor(ErlNifEnv *env, void *obj) {
  // Memory deallocation is done by Erlang GC since we released the resource
  // with `enif_release_resource`, so we only need to do object destruction.
  simdjson::dom::parser *pparser = (simdjson::dom::parser *)obj;
  pparser->~parser();
}

static ErlNifFunc nif_funcs[] = {
    {"parse", 2, nif_parse, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"new", 1, nif_new},
};

ERL_NIF_INIT(esimdjson, nif_funcs, load, 0, 0, 0);
