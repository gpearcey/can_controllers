rm -r docs/
mkdir docs/
cd docs/
doxygen ../Doxyfile
echo "<meta http-equiv=\"refresh\" content=\"0; url=html\">" > index.html