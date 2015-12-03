{
  "targets": [
    {
      "target_name": "DeviceHandle",
      "sources": [ "src/module.cpp"],
      "include_dirs": [
        "<!(node -e \"require('nan')\")","."
      ],
      'libraries': [ ]
    }
  ]
}