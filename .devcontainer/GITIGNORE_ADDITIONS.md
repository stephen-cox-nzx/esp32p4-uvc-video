# Suggested additions to your root .gitignore for ESP-IDF development

# Build artifacts
build/
sdkconfig.old
sdkconfig

# IDE and editor
.vscode/
.idea/
*.swp
*.swo
*~
.DS_Store
.project
.settings/

# Python cache
__pycache__/
*.py[cod]
*$py.class
*.so
.Python
env/
venv/
.pytest_cache/
.coverage
htmlcov/

# IDF Tools (if not using mounted cache)
.cache/
.idf_tools/

# Temporary files
*.tmp
*.log
*.bak

# OS-specific
Thumbs.db
.AppleDouble
.LSOverride

# ESP-IDF specific
sdkconfig.cmake
size_output/

# Debug and build outputs
*.elf
*.bin
*.map
