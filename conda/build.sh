#!/bin/bash
# build.sh - For Linux builds

set -ex

# Create the target directories
mkdir -p $PREFIX/bin
mkdir -p $PREFIX

# Copy the shared library and plugin file
cp bin/libplugify-plugin-polyhook.so $PREFIX/bin/
cp plugify-plugin-polyhook.pplugin $PREFIX/

# Set proper permissions
chmod 755 $PREFIX/bin/libplugify-plugin-polyhook.so
chmod 644 $PREFIX/plugify-plugin-polyhook.pplugin

# Create activation scripts for proper library path
mkdir -p $PREFIX/etc/conda/activate.d
mkdir -p $PREFIX/etc/conda/deactivate.d

cat > $PREFIX/etc/conda/activate.d/plugify-plugin-polyhook.sh << EOF
#!/bin/bash
export PLUGIFY_POLYHOOK_PLUGIN_PATH="\${CONDA_PREFIX}:\${PLUGIFY_POLYHOOK_PLUGIN_PATH}"
EOF

cat > $PREFIX/etc/conda/deactivate.d/plugify-plugin-polyhook.sh << EOF
#!/bin/bash
export PLUGIFY_POLYHOOK_PLUGIN_PATH="\${PLUGIFY_POLYHOOK_PLUGIN_PATH//\${CONDA_PREFIX}:/}"
EOF

chmod +x $PREFIX/etc/conda/activate.d/plugify-plugin-polyhook.sh
chmod +x $PREFIX/etc/conda/deactivate.d/plugify-plugin-polyhook.sh