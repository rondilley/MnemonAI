"""
LongMemEval Agent Infrastructure

Hardware detection, chat model management, ReAct agent loop with mnemond
MCP tools, and multi-judge evaluation panel.

Used by test_longmemeval.py -- not run directly.
"""

import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error


# =========================================================================
# Hardware Detection (mirrors mnemond's hardware.c)
# =========================================================================

def detect_hardware():
    """Detect CPU, GPU, RAM, and ROCm availability via sysfs/procfs.

    Returns dict with: cpu_model, cpu_cores, ram_total_gb, ram_avail_gb,
    gpu_vendor, gpu_model, gpu_vram_gb, gpu_gtt_gb, has_rocm.
    """
    hw = {
        "cpu_model": "", "cpu_cores": 1,
        "ram_total_gb": 0, "ram_avail_gb": 0,
        "gpu_vendor": "none", "gpu_model": "",
        "gpu_vram_gb": 0, "gpu_gtt_gb": 0,
        "has_rocm": False,
    }

    # CPU
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    hw["cpu_model"] = line.split(":", 1)[1].strip()
                    break
        hw["cpu_cores"] = os.cpu_count() or 1
    except OSError:
        pass

    # Memory
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    hw["ram_total_gb"] = kb / (1024 * 1024)
                elif line.startswith("MemAvailable:"):
                    kb = int(line.split()[1])
                    hw["ram_avail_gb"] = kb / (1024 * 1024)
    except OSError:
        pass

    # GPU via sysfs (AMD, Intel)
    try:
        drm_dir = "/sys/class/drm"
        for entry in sorted(os.listdir(drm_dir)):
            if not entry.startswith("card") or "-" in entry[4:]:
                continue
            dev = os.path.join(drm_dir, entry, "device")
            vendor_path = os.path.join(dev, "vendor")
            if not os.path.exists(vendor_path):
                continue
            with open(vendor_path) as f:
                vendor = f.read().strip()
            if vendor != "0x1002":  # AMD
                continue
            hw["gpu_vendor"] = "amd"
            # VRAM
            vram_path = os.path.join(dev, "mem_info_vram_total")
            if os.path.exists(vram_path):
                with open(vram_path) as f:
                    hw["gpu_vram_gb"] = int(f.read().strip()) / (1024**3)
            # GTT (GPU-accessible system RAM for APUs)
            gtt_path = os.path.join(dev, "mem_info_gtt_total")
            if os.path.exists(gtt_path):
                with open(gtt_path) as f:
                    hw["gpu_gtt_gb"] = int(f.read().strip()) / (1024**3)
            # GPU model from CPU model (APU) or vbios
            if "Radeon" in hw["cpu_model"]:
                m = re.search(r"Radeon.*", hw["cpu_model"])
                if m:
                    hw["gpu_model"] = f"AMD {m.group()}"
            hw["has_rocm"] = os.path.exists("/dev/kfd")
            break
    except OSError:
        pass

    # NVIDIA via nvidia-smi (fallback)
    if hw["gpu_vendor"] == "none":
        try:
            out = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=name,memory.total",
                 "--format=csv,noheader,nounits"],
                text=True, timeout=5).strip()
            if out:
                parts = out.split(",")
                hw["gpu_vendor"] = "nvidia"
                hw["gpu_model"] = parts[0].strip()
                hw["gpu_vram_gb"] = float(parts[1].strip()) / 1024
        except (subprocess.SubprocessError, FileNotFoundError, OSError):
            pass

    return hw


# =========================================================================
# Chat Model Catalog
# =========================================================================

# Models ordered by quality (best first). Each entry specifies minimum
# available memory (VRAM for discrete GPU, or total RAM for APU/CPU).
# All models support tool/function calling via their chat templates.
CHAT_MODELS = [
    # Gemma 4 requires llama.cpp built after 2026-04-03; listed first
    # but auto-selection verifies compatibility before choosing.
    {
        "name": "Gemma-4-26B-A4B-it (MoE, best quality)",
        "filename": "gemma-4-26B-A4B-it-Q4_K_M.gguf",
        "url": "https://huggingface.co/ggml-org/gemma-4-26b-a4b-it-GGUF/resolve/main/gemma-4-26B-A4B-it-Q4_K_M.gguf",
        "min_mem_gb": 20,
        "ctx_size": 8192,
        "gpu_layers": 99,
        "verify": True,  # Must pass a smoke test before use
    },
    {
        "name": "Qwen3-32B-Q4_K_M (recommended)",
        "filename": "Qwen3-32B-Q4_K_M.gguf",
        "url": "https://huggingface.co/unsloth/Qwen3-32B-GGUF/resolve/main/Qwen3-32B-Q4_K_M.gguf",
        "min_mem_gb": 22,
        "ctx_size": 8192,
        "gpu_layers": 99,
    },
    {
        "name": "Gemma-4-E4B-it (small, fast)",
        "filename": "gemma-4-e4b-it-Q4_K_M.gguf",
        "url": "https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-e4b-it-Q4_K_M.gguf",
        "min_mem_gb": 7,
        "ctx_size": 8192,
        "gpu_layers": 99,
        "verify": True,
    },
    {
        "name": "Qwen3-8B-Q4_K_M (fallback)",
        "filename": "Qwen3-8B-Q4_K_M.gguf",
        "url": "https://huggingface.co/unsloth/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf",
        "min_mem_gb": 6,
        "ctx_size": 8192,
        "gpu_layers": 99,
    },
    {
        "name": "Qwen3-4B-Q4_K_M (minimal)",
        "filename": "Qwen3-4B-Q4_K_M.gguf",
        "url": "https://huggingface.co/unsloth/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf",
        "min_mem_gb": 3,
        "ctx_size": 4096,
        "gpu_layers": 99,
    },
]

# Path to llama-completion binary (set by caller or auto-detected)
_LLAMA_BINARY = os.path.expanduser("~/.local/bin/llama-completion")


def _smoke_test_model(model_path, llama_binary=None):
    """Quick test that a model loads and generates without crashing."""
    binary = llama_binary or _LLAMA_BINARY
    if not os.path.exists(binary):
        return False
    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")
    try:
        result = subprocess.run(
            [binary, "-m", model_path, "-p", "Hi", "-n", "4",
             "--no-display-prompt", "-ngl", "0"],
            capture_output=True, text=True, timeout=30, env=env)
        return result.returncode == 0
    except (subprocess.SubprocessError, OSError):
        return False


def select_chat_model(hw, models_dir=None, llama_binary=None):
    """Pick the best chat model that fits the detected hardware.

    Prefers models already downloaded. Skips models that need verification
    (e.g., Gemma 4 with older llama.cpp) if they fail the smoke test.
    """
    # For AMD APUs, usable memory is GTT (GPU-accessible system RAM)
    # For discrete GPUs, use VRAM
    # For CPU-only, use available RAM
    if hw["gpu_vendor"] == "amd" and hw["gpu_gtt_gb"] > 0:
        avail = hw["gpu_gtt_gb"]
    elif hw["gpu_vram_gb"] > 0:
        avail = hw["gpu_vram_gb"]
    else:
        avail = hw["ram_avail_gb"]

    if not models_dir:
        models_dir = os.path.expanduser("~/.local/share/mnemond/models")

    for model in CHAT_MODELS:
        if avail < model["min_mem_gb"]:
            continue

        # Check if already downloaded
        path = os.path.join(models_dir, model["filename"])
        if not os.path.exists(path):
            continue

        # Verify compatibility if needed (e.g., Gemma 4 needs new llama.cpp)
        if model.get("verify"):
            if not _smoke_test_model(path, llama_binary):
                continue

        return model

    # No downloaded model found -- pick best that fits and will need download
    for model in CHAT_MODELS:
        if avail >= model["min_mem_gb"] and not model.get("verify"):
            return model

    return CHAT_MODELS[-1]


def ensure_chat_model(model_info, models_dir):
    """Download the chat model if not already present. Returns path.

    Uses wget with HTTP range-request resume to handle flaky SSL
    connections to HuggingFace CDN (OpenSSL 3.0 bug).
    """
    os.makedirs(models_dir, exist_ok=True)
    path = os.path.join(models_dir, model_info["filename"])

    if os.path.exists(path):
        size_gb = os.path.getsize(path) / (1024**3)
        if size_gb > 0.5:  # Sanity check -- at least 500MB
            return path

    print(f"  Downloading {model_info['name']}...")
    print(f"    URL: {model_info['url']}")
    print(f"    To:  {path}")

    # Resume loop: wget -c retries with HTTP Range requests.
    # HuggingFace CDN supports range requests, so each attempt
    # picks up where the last SSL failure left off.
    max_attempts = 2000  # Enough for ~20GB at ~2-20MB per attempt
    last_report = 0
    for attempt in range(max_attempts):
        current = os.path.getsize(path) if os.path.exists(path) else 0

        # Progress report every 100MB
        if current - last_report >= 100 * 1024 * 1024:
            print(f"    {current // (1024*1024)}MB downloaded...", flush=True)
            last_report = current

        try:
            result = subprocess.run(
                ["wget", "--no-check-certificate", "-c", "-q",
                 "-O", path, "-t", "1", "--timeout=30",
                 model_info["url"]],
                capture_output=True, text=True, timeout=120)
            if result.returncode == 0:
                break  # Success -- full download completed
        except subprocess.TimeoutExpired:
            pass  # wget timed out, resume will continue

    final_size = os.path.getsize(path) if os.path.exists(path) else 0
    if final_size < 100 * 1024 * 1024:  # Less than 100MB = failed
        raise RuntimeError(
            f"Download incomplete: {final_size // (1024*1024)}MB")

    print(f"    Done: {final_size // (1024*1024)}MB")
    return path


# =========================================================================
# ReAct Agent with mnemond MCP Tools
# =========================================================================

# System prompt for the ReAct agent.
# NOTE: Uses single braces in JSON examples -- this is a plain string,
# NOT an f-string, so { and } are literal.
AGENT_SYSTEM_PROMPT = """\
You are a personal memory assistant. You have a database of the user's \
past conversations with both memory search and a knowledge graph. Use \
the right tools to find information, then answer the question.

SEARCH TOOLS:

search_hybrid - Best general search. Combines keyword, semantic, and \
graph signals. Use this first for most questions.
  Action Input: {"query": "search terms", "top_k": 5}

search_keyword - Exact keyword/phrase match.
  Action Input: {"query": "exact phrase", "top_k": 5}

search_temporal - Find memories in a date range. Use for time-based questions.
  Action Input: {"since": "2023-01-01T00:00:00Z", "until": "2023-06-01T00:00:00Z", "top_k": 10}

KNOWLEDGE GRAPH TOOLS:

search_entities - Find people, technologies, places in the knowledge graph.
  Action Input: {"query": "person or thing name", "top_k": 5}

get_entity_graph - Get an entity and all its connections.
  Action Input: {"entity_id": "uuid-from-search_entities", "depth": 1, "max_nodes": 20}

EVENT TIMELINE TOOLS:

extract_events - Parse dates from text, create dated event entities.
  Action Input: {"content": "text with dates", "context_year": 2023}

search_events - Find events by their actual dates (not storage time).
  Action Input: {"since": "January 1", "until": "February 1", "top_k": 10}

calculate_duration - Compute days between two dates. ALWAYS use this \
for "how many days" questions instead of calculating yourself.
  Action Input: {"from": "January 10", "to": "January 17", "context_year": 2023}

HISTORY TOOLS:

get_changes_since - What changed after a date. Use for knowledge-update questions.
  Action Input: {"since": "2023-01-01T00:00:00Z", "top_k": 10}

retrieve_memory - Get full content of a specific memory by ID.
  Action Input: {"id": "memory-uuid"}

STRATEGY BY QUESTION TYPE:
- "What/Who/Where" facts: search_hybrid first, then search_keyword if needed.
- "Which came first" / time ordering: search_events to find dated events.
- "How many days between": search_hybrid to find dates, then calculate_duration.
  NEVER compute days yourself -- always use the calculate_duration tool.
- "What changed" / updates: get_changes_since or search_hybrid for both old and new.
- People/tech connections: search_entities + get_entity_graph.

FORMAT:

Thought: I should search for [topic]
Action: search_hybrid
Action Input: {"query": "topic", "top_k": 5}

After seeing Observation, continue or give your final answer:

Thought: Based on the search results, [reasoning about dates/facts]
Final Answer: [direct, concise answer]

RULES:
1. ALWAYS search before answering. Never guess.
2. Read [Session date: ...] headers carefully for temporal ordering.
3. Give a direct Final Answer. State facts, not hedges.
4. If not found after searching, say: I don't have that information.
5. You MUST give a Final Answer. Maximum 4 tool calls.\
"""

MAX_AGENT_TURNS = 6


def _clean_llm_output(text):
    """Strip noise from llama-completion output."""
    # Remove <think>...</think> blocks (Qwen3 extended thinking)
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL)
    # Remove orphan think tags
    text = re.sub(r'</?think>', '', text)
    # Remove special tokens
    text = re.sub(r'<\|[^|]*\|>', '', text)
    # Remove GDB/debugger noise
    text = re.sub(r'^\[(?:New LWP|Thread|Inferior|Detaching).*$', '',
                  text, flags=re.MULTILINE)
    text = re.sub(r'^(?:Using host|Debuginfod|This GDB).*$', '',
                  text, flags=re.MULTILINE)
    text = re.sub(r'^#\d+\s+0x[0-9a-f]+.*$', '', text, flags=re.MULTILINE)
    text = re.sub(r'^\d+\s+in \S+\.c$', '', text, flags=re.MULTILINE)
    # Remove EOF marker
    text = re.sub(r'>\s*EOF by user\s*$', '', text)
    # Collapse blank lines
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def _summarize_observation(raw_json_text, max_chars=12000):
    """Summarize a tool observation to fit in the agent's context window.

    Extracts session dates and key content from search results rather
    than dumping raw multi-turn conversations.
    """
    try:
        parsed = json.loads(raw_json_text)
    except (json.JSONDecodeError, TypeError):
        if len(raw_json_text) > max_chars:
            return raw_json_text[:max_chars] + "\n[truncated]"
        return raw_json_text

    results = parsed.get("results", [])
    if not results:
        return raw_json_text

    summaries = []
    chars_used = 0
    per_result_budget = max(max_chars // max(len(results), 1), 500)

    for r in results:
        content = r.get("content", "")
        rid = r.get("id", "?")

        # Extract session date and ID from headers
        session_date = ""
        session_id = ""
        for line in content.split("\n")[:5]:
            if line.startswith("[Session date:"):
                session_date = line
            elif line.startswith("[Session ID:"):
                session_id = line

        # Truncate content per result
        if len(content) > per_result_budget:
            content = content[:per_result_budget] + "\n[...]"

        entry = f"--- Memory {rid[:8]} ---\n"
        if session_date:
            entry += session_date + "\n"
        entry += content + "\n"
        summaries.append(entry)
        chars_used += len(entry)
        if chars_used > max_chars:
            summaries.append(f"[{len(results) - len(summaries)} more results omitted]")
            break

    return "\n".join(summaries)


class ReActAgent:
    """ReAct agent that uses a local LLM + mnemond MCP tools."""

    def __init__(self, mnemond_proc, llama_binary, model_path,
                 ctx_size=32768, gpu_layers=99):
        self.mnemond = mnemond_proc
        self.llama = llama_binary
        self.model = model_path
        self.ctx_size = ctx_size
        self.gpu_layers = gpu_layers
        self.req_id = 10000
        self.total_inference_ms = 0
        self.total_tool_ms = 0
        self.total_turns = 0

    def answer(self, question):
        """Run the agent loop. Returns (answer_text, timing_dict)."""
        conversation = f"Question: {question}\n\n"
        timing = {"inference_ms": 0, "tool_ms": 0, "turns": 0}

        for turn in range(MAX_AGENT_TURNS):
            t0 = time.monotonic()
            response = self._llm_complete(conversation)
            inference_ms = (time.monotonic() - t0) * 1000
            timing["inference_ms"] += inference_ms
            timing["turns"] += 1

            if not response:
                break

            conversation += response + "\n"

            # Check for Final Answer first
            final = self._extract_final_answer(response)
            if final is not None:
                self._accum(timing)
                return final, timing

            # Check for Action
            action, action_input = self._extract_action(response)
            if action:
                t0 = time.monotonic()
                observation = self._execute_tool(action, action_input)
                tool_ms = (time.monotonic() - t0) * 1000
                timing["tool_ms"] += tool_ms

                # Budget depends on context window
                obs_budget = min(self.ctx_size * 3, 50000)
                observation = _summarize_observation(observation, obs_budget)
                conversation += f"Observation: {observation}\n\n"
            else:
                # No valid action or answer -- force a final answer turn
                conversation += (
                    "You must now provide your Final Answer based on "
                    "what you have seen. Respond with:\n"
                    "Final Answer: [your answer]\n\n"
                )

        # Exhausted turns -- try to extract any answer from conversation
        # Look for the last Final Answer in the full conversation
        all_finals = re.findall(r'Final Answer:\s*(.+?)(?:\n|$)',
                                conversation)
        if all_finals:
            self._accum(timing)
            return all_finals[-1].strip(), timing

        self._accum(timing)
        return "I don't have that information.", timing

    def _accum(self, timing):
        self.total_inference_ms += timing["inference_ms"]
        self.total_tool_ms += timing["tool_ms"]
        self.total_turns += timing["turns"]

    def _llm_complete(self, conversation):
        """Call llama-completion with the full conversation."""
        env = os.environ.copy()
        home = os.path.expanduser("~")
        env["LD_LIBRARY_PATH"] = (
            f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", ""))

        prompt = conversation

        cmd = [
            self.llama,
            "-m", self.model,
            "-sys", AGENT_SYSTEM_PROMPT,
            "-p", prompt,
            "-n", "1024",
            "-c", str(self.ctx_size),
            "-ngl", str(self.gpu_layers),
            "--temp", "0.2",
            "--no-display-prompt",
            "-e",
        ]

        try:
            result = subprocess.run(
                cmd, capture_output=True, text=True,
                timeout=180, env=env)
            output = result.stdout.strip()
            output = _clean_llm_output(output)
            return output if output else None
        except (subprocess.TimeoutExpired, subprocess.SubprocessError):
            return None

    def _extract_final_answer(self, text):
        """Extract 'Final Answer: ...' from LLM output."""
        m = re.search(r'Final Answer:\s*(.+)', text, re.DOTALL)
        if m:
            answer = m.group(1).strip()
            # Stop at next Thought/Action if model kept going
            answer = re.split(r'\n(?:Thought|Action):', answer)[0].strip()
            return answer if answer else None
        return None

    def _extract_action(self, text):
        """Extract Action and Action Input from LLM output."""
        action_m = re.search(r'Action:\s*(\S+)', text)
        # Match JSON object -- greedy to handle nested braces
        input_m = re.search(r'Action Input:\s*(\{[^}]*\})', text)
        if action_m and input_m:
            raw = input_m.group(1)
            # Fix common LLM issues: doubled braces, trailing commas
            raw = raw.replace('{{', '{').replace('}}', '}')
            raw = re.sub(r',\s*}', '}', raw)
            try:
                args = json.loads(raw)
                return action_m.group(1).strip(), args
            except json.JSONDecodeError:
                pass
        # Fallback: action without proper input -- try with empty args
        if action_m and not input_m:
            return action_m.group(1).strip(), {}
        return None, None

    def _execute_tool(self, tool_name, arguments):
        """Execute an MCP tool against the mnemond process."""
        self.req_id += 1
        resp = _send_recv(self.mnemond, {
            "jsonrpc": "2.0", "id": self.req_id,
            "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments}
        })
        if not resp:
            return '{"error": "no response from mnemond"}'
        result = resp.get("result", {})
        is_error = result.get("isError", False)
        if is_error:
            content = result.get("content", [])
            if content:
                return content[0].get("text", '{"error": "tool error"}')
            return '{"error": "tool error"}'
        content = result.get("content", [])
        if content:
            return content[0].get("text", '{"results": []}')
        return '{"results": []}'


class ApiReActAgent(ReActAgent):
    """ReAct agent backed by an API LLM instead of local llama-completion.

    Same mnemond tools and ReAct loop, but uses an API (GPT-4o, Claude, etc.)
    for inference. Useful for comparing local vs API model quality while
    holding the retrieval system (mnemond) constant.
    """

    def __init__(self, mnemond_proc, api_name, ctx_size=128000):
        # Don't call super().__init__ with llama args
        self.mnemond = mnemond_proc
        self.api_name = api_name
        self.ctx_size = ctx_size
        self.gpu_layers = 0
        self.llama = None
        self.model = None
        self.req_id = 10000
        self.total_inference_ms = 0
        self.total_tool_ms = 0
        self.total_turns = 0

        # Load API config
        if api_name not in JUDGE_APIS:
            raise ValueError(f"Unknown API: {api_name}")
        self._api_config = JUDGE_APIS[api_name]
        self._api_key = load_api_key(self._api_config["key_file"])
        if not self._api_key:
            raise ValueError(f"API key not found: {self._api_config['key_file']}")

    def _llm_complete(self, conversation):
        """Call the API LLM with the system prompt + conversation."""
        prompt = AGENT_SYSTEM_PROMPT + "\n\n" + conversation
        try:
            result = _api_call(
                self._api_config, self._api_key, prompt,
                max_tokens=1024, temperature=0.2)
            return _clean_llm_output(result) if result else None
        except Exception:
            return None


def _send_recv(proc, request):
    """Send a JSON-RPC 2.0 request and read one response."""
    line = json.dumps(request) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        return None
    return json.loads(resp_line.strip())


def _call_tool(proc, name, arguments, req_id):
    """Call an MCP tool and return (result_dict, is_error)."""
    resp = _send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments}
    })
    if not resp:
        return {}, True
    result = resp.get("result", {})
    is_error = result.get("isError", False)
    content = result.get("content", [])
    if content:
        try:
            inner = json.loads(content[0].get("text", "{}"))
        except json.JSONDecodeError:
            inner = {}
    else:
        inner = {}
    return inner, is_error


# =========================================================================
# Ingestion Pipeline -- Entity Extraction + Knowledge Graph Building
# =========================================================================

_ENTITY_EXTRACTION_TEMPLATE = (
    'Extract entities and relationships from this conversation session.\n\n'
    'Return a JSON object with:\n'
    '- "entities": list of objects with keys: name, type '
    '(person|technology|organization|event|location|concept), '
    'observations (list of short facts)\n'
    '- "relations": list of objects with keys: source (entity name), '
    'target (entity name), type '
    '(uses|works_on|owns|attended|located_in|related_to), description\n\n'
    'Rules:\n'
    '- Only extract clearly stated facts, not inferences.\n'
    '- Use canonical names (e.g., "PostgreSQL" not "postgres").\n'
    '- Keep observations short and factual.\n'
    '- Maximum 10 entities and 10 relations per session.\n'
    '- Return ONLY valid JSON, no other text.\n\n'
    'Session content:\n{content}\n\n'
    'JSON:'
)


class IngestionPipeline:
    """Full ingestion pipeline: store memories + build knowledge graph.

    Steps per session:
    1. store_memory with created_at timestamp
    2. Extract entities and relations using the local LLM
    3. Create entities via create_entity + add_observation
    4. Link entities via create_relation
    5. After all sessions: consolidate_memories
    """

    def __init__(self, mnemond_proc, llama_binary, model_path,
                 gpu_layers=99, verbose=False):
        self.mnemond = mnemond_proc
        self.llama = llama_binary
        self.model = model_path
        self.gpu_layers = gpu_layers
        self.verbose = verbose
        self.req_id = 5000
        self.entity_ids = {}  # name -> entity UUID
        self.stats = {"memories": 0, "entities": 0, "relations": 0,
                      "extract_ms": 0, "graph_ms": 0}

    def ingest_session(self, content, session_id, session_date,
                       question_type, iso_date=None):
        """Ingest a single session: store memory + extract entities + events."""
        # Step 1: Store the memory
        self.req_id += 1
        store_args = {
            "content": content,
            "source_type": "conversation",
            "tags": ["longmemeval", question_type, f"session:{session_id}"],
        }
        if iso_date:
            store_args["created_at"] = iso_date
        r, ie = _call_tool(self.mnemond, "store_memory",
                           store_args, self.req_id)
        if not ie:
            self.stats["memories"] += 1

        # Step 2: Extract named entities (regex NER -- fast)
        extract_content = content[:3000]
        t0 = time.monotonic()
        extraction = self._extract_entities(extract_content)
        self.stats["extract_ms"] += (time.monotonic() - t0) * 1000

        if extraction:
            # Step 3: Create entities and relations in the knowledge graph
            t0 = time.monotonic()
            self._build_graph(extraction)
            self.stats["graph_ms"] += (time.monotonic() - t0) * 1000

        # Step 4: Extract dated events using mnemond's extract_events tool
        # This creates event entities with event_date timestamps for
        # temporal reasoning (search_events, calculate_duration)
        context_year = 2023  # LongMemEval default
        if iso_date:
            try:
                context_year = int(iso_date[:4])
            except (ValueError, IndexError):
                pass
        self.req_id += 1
        r, ie = _call_tool(self.mnemond, "extract_events", {
            "content": content[:8000],  # First 8K chars
            "context_year": context_year,
            "create_entities": True,
        }, self.req_id)
        if not ie:
            self.stats["entities"] += r.get("entities_created", 0)

    def finalize(self):
        """Run consolidation after all sessions are ingested."""
        self.req_id += 1
        _call_tool(self.mnemond, "consolidate_memories", {}, self.req_id)

    def _extract_entities(self, content):
        """Extract entities and relations using fast rule-based NER.

        Regex-based extraction is ~1000x faster than LLM-based and produces
        structured entities for the knowledge graph without consuming the
        inference budget. Extracts: capitalized names, technologies,
        dates/events, and infers basic relations.
        """
        entities = []
        relations = []
        seen_names = set()

        # Extract people (capitalized names in "User:" turns)
        for m in re.finditer(
                r'\b([A-Z][a-z]+(?:\s+[A-Z][a-z]+)?)\b'
                r'(?:\s+(?:is|was|works|joined|built|manages|owns|leads))',
                content):
            name = m.group(1)
            if name in ("User", "Assistant", "Session", "The"):
                continue
            if name not in seen_names:
                # Get surrounding context for observation
                start = max(0, m.start() - 10)
                end = min(len(content), m.end() + 100)
                ctx = content[start:end].split('\n')[0].strip()
                entities.append({
                    "name": name, "type": "person",
                    "observations": [ctx[:200]]
                })
                seen_names.add(name)

        # Extract technologies (known tech terms)
        tech_patterns = [
            r'\b(PostgreSQL|MySQL|MongoDB|Redis|SQLite|Elasticsearch|'
            r'Meilisearch|InfluxDB|DynamoDB|Cassandra)\b',
            r'\b(AWS|Azure|GCP|Kubernetes|Docker|Terraform|CloudFormation)\b',
            r'\b(React|Angular|Vue|Django|Flask|Express|FastAPI|Rails)\b',
            r'\b(Python|Rust|Go|Java|TypeScript|JavaScript|Ruby|C\+\+)\b',
            r'\b(Prometheus|Grafana|Loki|Datadog|Splunk|Jaeger)\b',
            r'\b(Jenkins|GitHub Actions|CircleCI|GitLab CI)\b',
            r'\b(Kafka|RabbitMQ|SQS|SNS|Celery|Airflow)\b',
            r'\b(Stripe|Keycloak|OAuth|JWT|Vault|Istio)\b',
            r'\b(nginx|Apache|Envoy|Calico|Cilium)\b',
            r'\b(VS Code|Neovim|IntelliJ|Emacs|Vim)\b',
            r'\b(Samsung Galaxy \w+|Dell XPS \w+|iPhone \w+|MacBook \w+)\b',
            r'\b(Toyota \w+|Honda \w+|Ford \w+|Tesla \w+)\b',
        ]
        for pattern in tech_patterns:
            for m in re.finditer(pattern, content):
                name = m.group(1)
                if name not in seen_names:
                    entities.append({
                        "name": name, "type": "technology",
                        "observations": [f"Mentioned in conversation"]
                    })
                    seen_names.add(name)

        # Extract events/activities
        event_patterns = [
            r'(?:attended|participated in|went to|joined)\s+'
            r'(?:the\s+|a\s+)?["\']?([A-Z][^"\',.]{5,60})["\']?',
            r'(?:workshop|conference|meetup|webinar|festival|event)'
            r'\s+(?:on|about|called)\s+["\']?([^"\',.]{5,60})["\']?',
        ]
        for pattern in event_patterns:
            for m in re.finditer(pattern, content, re.IGNORECASE):
                name = m.group(1).strip()
                if name not in seen_names and len(name) > 5:
                    entities.append({
                        "name": name, "type": "event",
                        "observations": [f"Event mentioned in conversation"]
                    })
                    seen_names.add(name)

        # Infer basic relations between people and technologies
        for ent in entities:
            if ent["type"] == "person":
                for tech in entities:
                    if tech["type"] == "technology":
                        # Check if person and tech appear near each other
                        p_pos = content.find(ent["name"])
                        t_pos = content.find(tech["name"])
                        if p_pos >= 0 and t_pos >= 0 and abs(p_pos - t_pos) < 300:
                            relations.append({
                                "source": ent["name"],
                                "target": tech["name"],
                                "type": "uses",
                                "description": f"{ent['name']} associated with {tech['name']}"
                            })

        if not entities:
            return None

        return {"entities": entities[:10], "relations": relations[:10]}

    def _build_graph(self, extraction):
        """Create entities and relations in mnemond's knowledge graph."""
        entities = extraction.get("entities", [])
        relations = extraction.get("relations", [])

        for ent in entities[:10]:  # Cap at 10
            name = ent.get("name", "").strip()
            etype = ent.get("type", "concept").strip()
            observations = ent.get("observations", [])
            if not name:
                continue

            # Check if entity already exists
            if name in self.entity_ids:
                # Add new observations to existing entity
                eid = self.entity_ids[name]
                for obs in observations[:5]:
                    self.req_id += 1
                    _call_tool(self.mnemond, "add_observation",
                               {"entity_id": eid, "observation": str(obs)},
                               self.req_id)
                continue

            # Create new entity
            self.req_id += 1
            r, ie = _call_tool(self.mnemond, "create_entity", {
                "name": name,
                "entity_type": etype,
                "observations": [str(o) for o in observations[:5]],
            }, self.req_id)

            if not ie and "id" in r:
                self.entity_ids[name] = r["id"]
                self.stats["entities"] += 1

        for rel in relations[:10]:  # Cap at 10
            src = rel.get("source", "").strip()
            tgt = rel.get("target", "").strip()
            rtype = rel.get("type", "related_to").strip()
            desc = rel.get("description", "")

            src_id = self.entity_ids.get(src)
            tgt_id = self.entity_ids.get(tgt)
            if not src_id or not tgt_id:
                continue

            self.req_id += 1
            r, ie = _call_tool(self.mnemond, "create_relation", {
                "source_id": src_id,
                "target_id": tgt_id,
                "edge_type": rtype,
                "description": str(desc),
            }, self.req_id)
            if not ie:
                self.stats["relations"] += 1


# =========================================================================
# Multi-Judge Panel
# =========================================================================

# API configurations for judge LLMs
JUDGE_APIS = {
    "gpt-4o": {
        "url": "https://api.openai.com/v1/chat/completions",
        "model": "gpt-4o-2024-08-06",
        "key_file": "openai.key.txt",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
    },
    "gpt-4o-mini": {
        "url": "https://api.openai.com/v1/chat/completions",
        "model": "gpt-4o-mini-2024-07-18",
        "key_file": "openai.key.txt",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
    },
    "claude": {
        "url": "https://api.anthropic.com/v1/messages",
        "model": "claude-sonnet-4-20250514",
        "key_file": "claude.key.txt",
        "auth_header": "x-api-key",
        "auth_prefix": "",
    },
    "xai": {
        "url": "https://api.x.ai/v1/chat/completions",
        "model": "grok-3-mini",
        "key_file": "xai.key.txt",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
    },
    "gemini": {
        "url": "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
        "model": "gemini-2.5-flash",
        "key_file": "gemini.key.txt",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
    },
    "mistral": {
        "url": "https://api.mistral.ai/v1/chat/completions",
        "model": "mistral-small-latest",
        "key_file": "mistral.key.txt",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
    },
}


def load_api_key(key_file):
    """Load API key from a key file in the project root."""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(test_dir)
    path = os.path.join(project_dir, key_file)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return f.read().strip()


def _api_call(config, api_key, prompt, max_tokens=10, temperature=0):
    """Make an API call to an OpenAI-compatible or Anthropic endpoint."""
    cfg = config

    if "anthropic" in cfg["url"]:
        body = json.dumps({
            "model": cfg["model"],
            "max_tokens": max_tokens,
            "temperature": temperature,
            "messages": [{"role": "user", "content": prompt}],
        }).encode()
        headers = {
            "Content-Type": "application/json",
            "anthropic-version": "2023-06-01",
            cfg["auth_header"]: cfg["auth_prefix"] + api_key,
        }
    else:
        body = json.dumps({
            "model": cfg["model"],
            "temperature": temperature,
            "max_tokens": max_tokens,
            "n": 1,
            "messages": [{"role": "user", "content": prompt}],
        }).encode()
        headers = {
            "Content-Type": "application/json",
            cfg["auth_header"]: cfg["auth_prefix"] + api_key,
        }

    req = urllib.request.Request(cfg["url"], data=body,
                                headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.loads(resp.read())

    if "anthropic" in cfg["url"]:
        return data["content"][0]["text"]
    else:
        return data["choices"][0]["message"]["content"]


# Judge prompt templates (from official LongMemEval evaluation)
JUDGE_PROMPTS = {
    "generic": (
        "I will give you a question, a correct answer, and a response from a "
        "model. Please answer yes if the response contains the correct answer. "
        "Otherwise, answer no. If the response is equivalent to the correct "
        "answer or contains all the intermediate steps to get the correct "
        "answer, you should also answer yes. If the response only contains a "
        "subset of the information required by the answer, answer no. \n\n"
        "Question: {question}\n\nCorrect Answer: {answer}\n\n"
        "Model Response: {response}\n\n"
        "Is the model response correct? Answer yes or no only."
    ),
    "temporal": (
        "I will give you a question, a correct answer, and a response from a "
        "model. Please answer yes if the response contains the correct answer. "
        "Otherwise, answer no. If the response is equivalent to the correct "
        "answer or contains all the intermediate steps to get the correct "
        "answer, you should also answer yes. If the response only contains a "
        "subset of the information required by the answer, answer no. In "
        "addition, do not penalize off-by-one errors for the number of days. "
        "If the question asks for the number of days/weeks/months, etc., and "
        "the model makes off-by-one errors (e.g., predicting 19 days when the "
        "answer is 18), the model's response is still correct. \n\n"
        "Question: {question}\n\nCorrect Answer: {answer}\n\n"
        "Model Response: {response}\n\n"
        "Is the model response correct? Answer yes or no only."
    ),
    "knowledge-update": (
        "I will give you a question, a correct answer, and a response from a "
        "model. Please answer yes if the response contains the correct answer. "
        "Otherwise, answer no. If the response contains some previous "
        "information along with an updated answer, the response should be "
        "considered as correct as long as the updated answer is the required "
        "answer.\n\n"
        "Question: {question}\n\nCorrect Answer: {answer}\n\n"
        "Model Response: {response}\n\n"
        "Is the model response correct? Answer yes or no only."
    ),
    "preference": (
        "I will give you a question, a rubric for desired personalized "
        "response, and a response from a model. Please answer yes if the "
        "response satisfies the desired response. Otherwise, answer no. The "
        "model does not need to reflect all the points in the rubric. The "
        "response is correct as long as it recalls and utilizes the user's "
        "personal information correctly.\n\n"
        "Question: {question}\n\nRubric: {answer}\n\n"
        "Model Response: {response}\n\n"
        "Is the model response correct? Answer yes or no only."
    ),
    "abstention": (
        "I will give you an unanswerable question, an explanation, and a "
        "response from a model. Please answer yes if the model correctly "
        "identifies the question as unanswerable. The model could say that the "
        "information is incomplete, or some other information is given but the "
        "asked information is not.\n\n"
        "Question: {question}\n\nExplanation: {answer}\n\n"
        "Model Response: {response}\n\n"
        "Does the model correctly identify the question as unanswerable? "
        "Answer yes or no only."
    ),
}


def _select_judge_prompt(question_type, question_id):
    """Select the appropriate judge prompt template key."""
    if "_abs" in question_id:
        return "abstention"
    elif question_type == "temporal-reasoning":
        return "temporal"
    elif question_type == "knowledge-update":
        return "knowledge-update"
    elif question_type == "single-session-preference":
        return "preference"
    return "generic"


class JudgePanel:
    """Multi-LLM judge panel with majority vote."""

    def __init__(self, judge_names=None):
        """Initialize with a list of judge names.

        If None, auto-detect available judges from API key files.
        """
        self.judges = {}  # name -> (config, api_key)
        self.calls = 0
        self.errors = 0
        self.timing_ms = 0

        if judge_names is None:
            # Auto-detect: use all judges with available keys
            judge_names = list(JUDGE_APIS.keys())

        for name in judge_names:
            if name not in JUDGE_APIS:
                continue
            key = load_api_key(JUDGE_APIS[name]["key_file"])
            if key:
                self.judges[name] = (JUDGE_APIS[name], key)

        if not self.judges:
            raise ValueError("No judge API keys found")

    @property
    def names(self):
        return list(self.judges.keys())

    def evaluate(self, question_type, question_id, question, answer,
                 model_response):
        """Evaluate using majority vote across all judges.

        Returns dict with:
          - correct: bool (majority vote)
          - votes: dict of judge_name -> bool
          - agree: int (number of agreeing judges)
          - total: int (number of judges that responded)
        """
        template_key = _select_judge_prompt(question_type, question_id)
        prompt = JUDGE_PROMPTS[template_key].format(
            question=question, answer=str(answer), response=model_response)

        votes = {}
        t0 = time.monotonic()

        for name, (config, api_key) in self.judges.items():
            try:
                result = _api_call(config, api_key, prompt,
                                   max_tokens=10, temperature=0)
                self.calls += 1
                votes[name] = "yes" in result.lower()
            except Exception:
                self.errors += 1
                # Judge failed -- skip this vote

        self.timing_ms += (time.monotonic() - t0) * 1000

        if not votes:
            return {"correct": False, "votes": {}, "agree": 0, "total": 0}

        yes_count = sum(1 for v in votes.values() if v)
        no_count = len(votes) - yes_count
        majority = yes_count > no_count

        return {
            "correct": majority,
            "votes": votes,
            "agree": max(yes_count, no_count),
            "total": len(votes),
        }
