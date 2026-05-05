# Ollama CLI Commands Guide

This guide provides useful commands to **check, monitor, and manage Ollama** from the terminal.

---

# 1. Check Ollama Installation

```bash
ollama --version
```

Verify Ollama is installed and available in PATH.

---

# 2. Check Ollama Status

```bash
ollama list
```

* Shows all downloaded models
* Confirms Ollama is working properly

---

# 3. Check Running Models

```bash
ollama ps
```

Displays:

* Running models
* Memory usage
* Active sessions

---

# 4. List Installed Models

```bash
ollama list
```

Example output:

```text
NAME            SIZE     MODIFIED
llama3          4.7GB    2 days ago
mistral         4.1GB    5 days ago
```

---

# 5. Pull (Download) Model

```bash
ollama pull llama3
```

---

# 6. Remove Model

```bash
ollama rm llama3
```

---

# 7. Run Model

```bash
ollama run llama3
```

Run with prompt:

```bash
ollama run llama3 "Hello, how are you?"
```

---

# 8. Stop Running Model

```bash
ollama stop llama3
```

---

# 9. Show Model Info

```bash
ollama show llama3
```

Displays:

* Model details
* Parameters
* Architecture

---

# 10. Check Logs

## macOS / Linux

```bash
tail -f ~/.ollama/logs/server.log
```

## Windows

```bash
type %USERPROFILE%\.ollama\logs\server.log
```

---

# 11. Check Ollama Server

By default Ollama runs on:

```text
http://localhost:11434
```

Test with curl:

```bash
curl http://localhost:11434
```

---

# 12. Restart Ollama Service

## macOS

```bash
killall ollama
ollama serve
```

## Linux (systemd)

```bash
sudo systemctl restart ollama
```

## Windows

* Restart from Task Manager
* Or restart terminal session

---

# 13. Check Port Usage

```bash
netstat -ano | findstr 11434
```

or (Linux/macOS):

```bash
lsof -i :11434
```

---

# 14. Run Ollama as Server

```bash
ollama serve
```

---

# 15. Test API Endpoint

```bash
curl http://localhost:11434/api/tags
```

---

# 16. Advanced Debug

Run with verbose logs:

```bash
OLLAMA_DEBUG=1 ollama serve
```

---

# 17. Common Troubleshooting

## Command not found

* Ensure Ollama is installed
* Add Ollama to PATH

---

## Model not found

```bash
ollama pull <model_name>
```

---

## Port already in use

```bash
netstat -ano | findstr 11434
```

Kill process if needed.

---

## High memory usage

```bash
ollama ps
```

Stop unused models:

```bash
ollama stop <model>
```

---

# 18. Quick Cheat Sheet

```bash
ollama --version
ollama list
ollama ps
ollama pull <model>
ollama run <model>
ollama stop <model>
ollama rm <model>
ollama show <model>
ollama serve
```

---

# Final Notes

* Default port: **11434**
* Works offline after model download
* Models can be several GB → ensure enough disk space

---
