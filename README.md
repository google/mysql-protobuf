This tree contains a work-in-progress version of MySQL that has
support for [Protocol Buffers](https://developers.google.com/protocol-buffers/).

The contributed code is licensed under GPLv2.

# Getting started
1. Install the following packages:  
   * On Debian/Ubuntu/Linux Mint run:  
     ```sudo apt-get install -y git g++ cmake bison libncurses5-dev```
   * On Red Hat/Fedora/CentOS run:  
     ```sudo yum install git g++ cmake bison ncurses-devel```
1. Clone this repo:  
  ```git clone git@github.com:google/mysql-protobuf.git```

1. Go to mysql-protobuf folder and create a new `bin` folder:  
  ```cd mysql-protobuf && mkdir bin && cd bin```

1. Compile the project. For that, you have to run `cmake` and then `make`:  
  ```cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=~/boost -DENABLE_DOWNLOADS=1 && make -j8```

1. Create a folder where you'll run `mysqld`:  
  ```mkdir -p ~/mysql-run && cd ~/mysql-run```

1. Copy `mysqld` to this folder:  
  ```cp /path/to/mysql-protobuf/bin/sql/mysqld .```

1. Copy ```errmsg.sys``` file to `share` folder:  
  ``` mkdir -p share && cp /path/to/mysql-protobuf/bin/sql/share/english/errmsg.sys share/```

1. Create a `tmpdir` folder:  
  ```mkdir -p tmpdir```

1. Initialize `mysqld`:  
  ```./mysqld --initialize-insecure --pid-file=./mysql.pid --basedir=. --datadir=./datadir --tmpdir=$HOME/mysql-run/tmpdir --socket=$HOME/mysql-run/mysqld.sock```

1. Run `mysqld`:  
   ```./mysqld --pid-file=./mysql.pid --basedir=. --datadir=./datadir --tmpdir=$HOME/mysql-run/tmpdir --socket=$HOME/mysql-run/mysqld.sock```

1. Open another terminal and connect to `mysqld`:  
   ```/path/to/mysql-protobuf/bin/client/mysql -u root -S ~/mysql-run/mysqld.sock```

For examples of usage, please see our [wiki](http://github.com/google/mysql-protobuf/wiki).
