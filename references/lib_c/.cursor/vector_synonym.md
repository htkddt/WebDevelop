# Vector Synonym Discovery — Design

## Goal
Improve `vector_generate_custom` (built-in hash) so that different names for the same place produce the same or similar vectors.

## Current Problem
```
"Saigon"           → hash → vector A
"Ho Chi Minh City" → hash → vector B  (completely different)
"Sài Gòn"          → hash → vector C  (completely different)
```

## Solution: LLM Alias Discovery at geo_learning insert time

### Flow
```
1. Geo worker extracts place: "Sài Gòn"

2. Ask gemma4:26b (local Ollama): 
   "List all known names, aliases, abbreviations for this place.
    Include: official name, common name, historical name, abbreviation, 
    Vietnamese with and without accents, English translation.
    Return JSON array of strings only."
   
   → gemma4 returns: ["Sài Gòn", "Saigon", "Ho Chi Minh City", "TPHCM", 
                       "TP.HCM", "Thành phố Hồ Chí Minh", "tp_ho_chi_minh"]

3. Normalize all → store in synonym table:
   "saigon" → "ho_chi_minh_city"
   "sài_gòn" → "ho_chi_minh_city"  
   "tphcm" → "ho_chi_minh_city"
   "tp.hcm" → "ho_chi_minh_city"
   "thành_phố_hồ_chí_minh" → "ho_chi_minh_city"

4. vector_generate_custom("Saigon")
   → synonym lookup: "saigon" → "ho_chi_minh_city"
   → hash "ho_chi_minh_city" → vector
   Same as vector_generate_custom("TPHCM") → same canonical → same vector
```

### Components

1. **Synonym table** (`m4_ht_t` in `api_context_t`)
   - Key: normalized alias string
   - Value: canonical name string
   - Loaded from Mongo geo_atlas at init
   - Updated by geo_learning worker on each insert

2. **Alias prompt** (in `geo_learning.c` `process_turn`)
   - After entity extraction, before embedding
   - One extra Ollama call per NEW entity (not per chat)
   - Model: uses Ollama default (gemma4:26b)
   - Parse JSON array response → normalize → insert synonyms

3. **Synonym lookup in vector_generate** 
   - Before hashing, check synonym table
   - If found: use canonical form for hashing
   - If not found: hash as-is (current behavior)

4. **Built-in synonyms** (hardcoded, small table)
   - Common Vietnamese ↔ English: "thành phố"="city", "quận"="district", "phường"="ward"
   - Not dependent on Mongo or Ollama

5. **Persistence**
   - Synonyms stored in Mongo `geo_atlas` as `aliases: [...]` array per document
   - Loaded into memory at `api_create` → `engine_init`
   - Survives restart

### Improvements to vector_generate_custom
- Add character trigrams (handles accent variants via partial overlap)
- Increase dimension from 384 to 768 (fewer hash collisions)
- Synonym lookup before hashing (canonical form matching)

### Config
- Uses Ollama default model (gemma4:26b or whatever is set)
- Alias discovery runs in geo_learning worker (async, non-blocking)
- `M4_GEO_ALIAS_DISCOVERY=0` env to disable (default: enabled when geo_learning runs)

### Cost
- One extra Ollama call per NEW geo entity (not per chat turn)
- Geo worker already calls Ollama for extraction — this adds ~1-2 seconds per entity
- 26B model is smart enough for accurate aliases
- No cloud API cost (local only)
