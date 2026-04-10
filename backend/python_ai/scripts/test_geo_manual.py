#!/usr/bin/env python3
"""
Manual test for geo_learning: one chat with a HCMC landmark, wait, then print geo_atlas.
Usage: from python_ai/: python3 scripts/test_geo_manual.py
Requires: c-lib built, Ollama running. With Mongo (USE_MONGOC=1) landmarks are stored.
"""
import ctypes
import sys
import time

sys.path.insert(0, ".")
from engine_ctypes import (
    load_lib,
    ApiOptions,
    M4ENGINE_MODE_MONGO_REDIS_ELK,
    API_DEFAULT_TENANT_ID,
    OL_BUF_SIZE,
)


def main():
    lib = load_lib()
    opts = ApiOptions(
        mode=M4ENGINE_MODE_MONGO_REDIS_ELK,
        mongo_uri=b"mongodb://127.0.0.1:27017",
        redis_host=b"127.0.0.1",
        redis_port=6379,
        es_host=None,
        es_port=0,
        log_db=None,
        log_coll=None,
        context_batch_size=0,
        smart_topic_opts=None,
        inject_geo_knowledge=0,
        disable_auto_system_time=0,
        geo_authority=0,
        model_switch_opts=None,
        vector_gen_backend=0,
        vector_ollama_model=None,
        embed_migration_autostart=0,
        session_idle_seconds=0,
        shared_collection_mongo_uri=None,
        shared_collection_json_path=None,
        shared_collection_backfill_db=None,
        learning_terms_path=None,
        enable_learning_terms=0,
        defer_learning_terms_load=0,
    )
    ctx = lib.api_create(ctypes.byref(opts))
    if not ctx:
        print("api_create failed")
        return 1
    print("Context created (MONGO_REDIS_ELK). Sending chat with HCMC landmark...")
    msg = "Ngã tư Hàng Xanh is in Bình Thạnh district."
    reply_buf = ctypes.create_string_buffer(OL_BUF_SIZE)
    rc = lib.api_chat(
        ctx,
        API_DEFAULT_TENANT_ID,
        API_DEFAULT_TENANT_ID,
        msg.encode("utf-8"),
        reply_buf,
        OL_BUF_SIZE,
    )
    if rc != 0:
        print("api_chat failed (is Ollama running?)")
        lib.api_destroy(ctx)
        return 1
    reply = reply_buf.value.decode("utf-8", errors="replace").strip()
    print(f"Reply: {reply[:200]}...")
    print("Waiting 25s for geo_learning worker (Ollama extraction + insert)...")
    time.sleep(25)
    landmarks_buf = ctypes.create_string_buffer(4096)
    n = lib.api_get_geo_atlas_landmarks(ctx, landmarks_buf, 4096)
    lib.api_destroy(ctx)
    if n > 0:
        s = landmarks_buf.value.decode("utf-8", errors="replace")
        print(f"geo_atlas landmarks ({n} bytes):\n{s}")
    else:
        print("geo_atlas empty. To persist: build c-lib with USE_MONGOC=1 (make clean && USE_MONGOC=1 make) and run MongoDB.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
