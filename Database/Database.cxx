//*-- Author :    Ole Hansen   1-Jun-2021

//////////////////////////////////////////////////////////////////////////
//
// Podd
//
// Database support functions.
// These functions were previously part of the THaAnalysisObject class.
//
//////////////////////////////////////////////////////////////////////////

#include "Database.h"
#include "TDatime.h"
#include "TObjArray.h"
#include "TObjString.h"
#include "TString.h"
#include "TError.h"
#include "TSystem.h"
#include "TThread.h"
#include "TVirtualMutex.h"

#include <cerrno>
#include <cctype>    // for isspace
#include <cstring>
#include <cstdlib>   // for atof, atoi
#include <iterator>  // for std::distance
#include <iostream>
#include <sstream>
#include <limits>
#include <algorithm>
#include <map>

using namespace std;

// Mutex for concurrent access to global Here function
static TVirtualMutex* gHereMutex = nullptr;

//_____________________________________________________________________________
const char* Here( const char* method, const char* prefix )
{
  // Utility function for error messages. The return value points to a static
  // string buffer that is unique to the current thread.
  // There are two usage cases:
  // ::Here("method","prefix")        -> returns ("prefix")::method
  // ::Here("Class::method","prefix") -> returns Class("prefix")::method

  // One static string buffer per thread ID
  static map<Long_t,TString> buffers;

  TString txt;
  if( prefix && *prefix ) {
    TString full_prefix(prefix);
    // delete trailing dot of prefix, if any
    if( full_prefix.EndsWith(".") )
      full_prefix.Chop();
    full_prefix.Prepend("(\""); full_prefix.Append("\")");
    const char* scope = nullptr;
    if( method && *method && (scope = strstr(method, "::")) ) {
      assert( scope >= method );
      auto pos = static_cast<Ssiz_t>(std::distance(method,scope));
      txt = method;
      assert(pos >= 0 && pos < txt.Length());
      txt.Insert(pos, full_prefix);
      method = nullptr;
    } else {
      txt = full_prefix + "::";
    }
  }
  if( method )
    txt.Append(method);

  R__LOCKGUARD2(gHereMutex);

  TString& ret = (buffers[ TThread::SelfId() ] = txt);

  return ret.Data(); // pointer to the C-string of a TString in static map
}

//=============================================================================
namespace Podd {

//_____________________________________________________________________________
TString& GetObjArrayString( const TObjArray* array, Int_t i )
{
  // Get the string at index i in the given TObjArray

  return (static_cast<TObjString*>(array->At(i)))->String();
}

//_____________________________________________________________________________
vector<string> GetDBFileList( const char* name, const TDatime& date, const char* here )
{
  // Return the database file searchlist as a vector of strings.
  // The file names are relative to the current directory.

  static const string defaultdir = "DEFAULT";
#ifdef WIN32
  static const string dirsep = "\\", allsep = "/\\";
#else
  static const string dirsep = "/", allsep = "/";
#endif

  vector<string> fnames;
  if( !name || !*name )
    return fnames;

  // If name contains a directory separator, we take the name verbatim
  string filename = name;
  if( filename.find_first_of(allsep) != string::npos ) {
    fnames.push_back( filename );
    return fnames;
  }

  // Build search list of directories
  vector<string> dnames;
  if( const char* dbdir = gSystem->Getenv("DB_DIR"))
    dnames.emplace_back(dbdir);
  dnames.emplace_back("DB");
  dnames.emplace_back("db");
  dnames.emplace_back(".");

  // Try to open the database directories in the search list.
  // The first directory that can be opened is taken as the database
  // directory. Subsequent directories are ignored.
  auto it = dnames.begin();
  void* dirp = nullptr;
  while( !(dirp = gSystem->OpenDirectory((*it).c_str())) &&
         (++it != dnames.end()) ) {}

  // None of the directories can be opened?
  if( it == dnames.end() ) {
    ::Error( here, "Cannot open any database directories. Check your disk!");
    return fnames;
  }

  // Pointer to database directory string
  string thedir = *it;

  // In the database directory, get the names of all subdirectories matching
  // a YYYYMMDD pattern.
  vector<string> time_dirs;
  bool have_defaultdir = false;
  while( const char* result = gSystem->GetDirEntry(dirp) ) {
    string item = result;
    if( item.length() == 8 ) {
      Int_t pos = 0;
      for( ; pos<8; ++pos )
        if( !isdigit(item[pos])) break;
      if( pos==8 )
        time_dirs.push_back( item );
    } else if ( item == defaultdir )
      have_defaultdir = true;
  }
  gSystem->FreeDirectory(dirp);

  // Search a date-coded subdirectory that corresponds to the requested date.
  bool found = false;
  if( !time_dirs.empty() ) {
    sort( time_dirs.begin(), time_dirs.end() );
    for( it = time_dirs.begin(); it != time_dirs.end(); ++it ) {
      Int_t item_date = atoi((*it).c_str());
      if( it == time_dirs.begin() && date.GetDate() < item_date )
        break;
      if( it != time_dirs.begin() && date.GetDate() < item_date ) {
        --it;
        found = true;
        break;
      }
      // Assume that the last directory is valid until infinity.
      if( it + 1 == time_dirs.end() && date.GetDate() >= item_date ) {
        found = true;
        break;
      }
    }
  }

  // Construct the database file name. It is of the form db_<prefix>.dat.
  // Subdetectors use the same files as their parent detectors!
  // If filename does not start with "db_", make it so
  if( filename.substr(0,3) != "db_" )
    filename.insert(0,"db_");
    // If filename does not end with ".dat", make it so
#ifndef NDEBUG
  // should never happen
  assert( filename.length() >= 4 );
#else
  if( filename.length() < 4 ) { fnames.clear(); return fnames; }
#endif
  if( *filename.rbegin() == '.' ) {
    filename += "dat";
  } else if( filename.substr(filename.length()-4) != ".dat" ) {
    filename += ".dat";
  }

  // Build the searchlist of file names in the order:
  // ./filename <dbdir>/<date-dir>/filename
  //    <dbdir>/DEFAULT/filename <dbdir>/filename
  fnames.push_back( filename );
  if( found ) {
    string item = thedir + dirsep + *it + dirsep + filename;
    fnames.push_back( item );
  }
  if( have_defaultdir ) {
    string item = thedir + dirsep + defaultdir + dirsep + filename;
    fnames.push_back( item );
  }
  fnames.push_back( thedir + dirsep + filename );

  return fnames;
}

//_____________________________________________________________________________
FILE* OpenDBFile( const char* name, const TDatime& date,  const char* here,
                  const char* filemode, int debug_flag, const char*& openpath )
{
  // Open database file and return a pointer to the C-style file descriptor.

  // Ensure input is sane
  if( !name || !*name )
    return nullptr;
  if( !here )
    here = "";
  if( !filemode )
    filemode = "r";
  openpath = nullptr;

  // Get list of database file candidates and try to open them in turn
  FILE* fi = nullptr;
  vector <string> fnames(GetDBFileList(name, date, here));
  if( !fnames.empty() ) {
    auto it = fnames.begin();
    do {
      if( debug_flag > 1 )
        cout << "Info in <" << here << ">: Opening database file " << *it;
      // Open the database file
      fi = fopen((*it).c_str(), filemode);

      if( debug_flag > 1 )
        if( !fi ) cout << " ... failed" << endl;
        else      cout << " ... ok" << endl;
      else if( debug_flag > 0 && fi )
        cout << "<" << here << ">: Opened database file " << *it << endl;
      // continue until we succeed
    } while( !fi && ++it != fnames.end() );
    if( fi )
      openpath = (*it).c_str();
  }
  if( !fi && debug_flag > 0 ) {
    ::Error(here, "Cannot open database file db_%s%sdat", name,
            (name[strlen(name) - 1] == '.' ? "" : "."));
  }

  return fi;
}

//_____________________________________________________________________________
FILE* OpenDBFile( const char* name, const TDatime& date, const char* here,
                  const char* filemode, int debug_flag )
{
  const char* openpath = nullptr;
  return OpenDBFile(name, date, here, filemode, debug_flag, openpath);
}

//---------- Database utility functions ---------------------------------------

//FIXME: make thread-safe
static string errtxt;
static int loaddb_depth = 0; // Recursion depth in LoadDB
static string loaddb_prefix; // Actual prefix of object in LoadDB (for err msg)

// Local helper functions
namespace {
//_____________________________________________________________________________
Int_t IsDBdate( const string& line, TDatime& date, bool warn = true )
{
  // Check if 'line' contains a valid database time stamp. If so,
  // parse the line, set 'date' to the extracted time stamp, and return 1.
  // Else return 0;
  // Time stamps must be in SQL format: [ yyyy-mm-dd hh:mi:ss ]

  auto lbrk = line.find('[');
  if( lbrk == string::npos || lbrk >= line.size() - 12 ) return 0;
  auto rbrk = line.find(']', lbrk);
  if( rbrk == string::npos || rbrk <= lbrk + 11 ) return 0;
  Int_t yy, mm, dd, hh, mi, ss;
  if( sscanf(line.substr(lbrk + 1, rbrk - lbrk - 1).c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
             &yy, &mm, &dd, &hh, &mi, &ss) != 6
      || yy < 1995 || mm < 1 || mm > 12 || dd < 1 || dd > 31
      || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59 ) {
    if( warn )
      ::Warning("IsDBdate()",
                "Invalid date tag %s", line.c_str());
    return 0;
  }
  date.Set(yy, mm, dd, hh, mi, ss);
  return 1;
}

//_____________________________________________________________________________
Int_t IsDBkey( const string& line, const char* key, string& text )
{
  // Check if 'line' is of the form "key = value" and, if so, whether the key
  // equals 'key'. Keys are not case sensitive.
  // - If there is no '=', then return 0.
  // - If there is a '=', but the left-hand side doesn't match 'key',
  //   then return -1.
  // - If key found, parse the line, set 'text' to the whitespace-trimmed
  //   text after the "=" and return +1.
  // 'text' is not changed unless a valid key is found.
  //
  // Note: By construction in ReadDBline, 'line' is not empty, any comments
  // starting with '#' have been removed, and trailing whitespace has been
  // trimmed. Also, all tabs have been converted to spaces.

  // Search for "="
  const char* ln = line.c_str();
  const char* eq = strchr(ln, '=');
  if( !eq ) return 0;
  // Extract the key
  while( *ln == ' ' ) ++ln; // find_first_not_of(" ")
  assert(ln <= eq);
  if( ln == eq ) return -1;
  const char* p = eq - 1;
  assert(p >= ln);
  while( *p == ' ' ) --p; // find_last_not_of(" ")
  if( strncmp(ln, key, p - ln + 1) != 0 ) return -1;
  // Key matches. Now extract the value, trimming leading whitespace.
  ln = eq + 1;
  assert(!*ln || *(ln + strlen(ln) - 1) != ' '); // Trailing space already trimmed
  while( *ln == ' ' ) ++ln;
  text = ln;

  return 1;
}

//_____________________________________________________________________________
inline Int_t ChopPrefix( string& s )
{
  // Remove trailing level from prefix. Example "L.vdc." -> "L."
  // Return remaining number of dots, or zero if empty/invalid prefix

  auto len = s.size();
  if( len >= 2 ) {
    auto pos = s.rfind('.', len - 2);
    if( pos != string::npos ) {
      s.erase(pos + 1);
      auto ndot = std::count(s.begin(), s.end(), '.');
      if( ndot <= kMaxInt )
        return static_cast<Int_t>(ndot);
    }
  }
  s.clear();
  return 0;
}

//_____________________________________________________________________________
inline bool IsTag( const char* buf )
{
  // Return true if the string in 'buf' matches regexp ".*\[.+\].*",
  // i.e. it is a database section marker.  Generic utility function.

  const char* p = buf;
  while( *p && *p != '[' ) p++;
  if( !*p ) return false;
  p++;
  if( !*p || *p == ']' ) return false;
  p++;
  while( *p && *p != ']' ) p++;
  return (*p == ']');
}

//_____________________________________________________________________________
Int_t GetLine( FILE* file, char* buf, Int_t bufsiz, string& line )
{
  // Get a line (possibly longer than 'bufsiz') from 'file' using
  // using the provided buffer 'buf'. Put result into string 'line'.
  // This is similar to std::getline, except that C-style I/O is used.
  // Also, convert all tabs to spaces.
  // Returns 0 on success, or EOF if no more data (or error).

  char* r = nullptr;
  line.clear();
  while( (r = fgets(buf, bufsiz, file)) ) {
    char* c = strchr(buf, '\n');
    if( c )
      *c = '\0';
    // Convert all tabs to spaces
    char* p = buf;
    while( (p = strchr(p, '\t')) ) *(p++) = ' ';
    // Append to string
    line.append(buf);
    // If newline was read, the line is finished
    if( c )
      break;
  }
  // Don't report EOF if we have any data
  if( !r && line.empty() )
    return EOF;
  return 0;
}

//_____________________________________________________________________________
inline Bool_t IsAssignment( const string& str )
{
  // Check if 'str' has the form of an assignment (<text> = [optional text]).
  // Properly handles comparison operators '==', '!=', '<=', '>='

  string::size_type pos = str.find('=');
  if( pos == string::npos )
    // No '='
    return false;
  if( str.find_first_not_of(" \t") == pos )
    // Only whitespace before '=' or '=' at start of line
    return false;
  assert(pos > 0);
  // '!=', '<=', '>=' or '=='
  return !(str[pos - 1] == '!' || str[pos - 1] == '<' || str[pos - 1] == '>' ||
           (pos + 1 < str.length() && str[pos + 1] == '='));
}

} // end anonymous namespace

//_____________________________________________________________________________
Int_t ReadDBline( FILE* file, char* buf, Int_t bufsiz, string& line )
{
  // Get a text line from the database file 'file'. Ignore all comments
  // (anything after a #). Trim trailing whitespace. Concatenate continuation
  // lines (ending with \).
  // Only returns if a non-empty line was found, or on EOF.

  line.clear();

  Int_t r = 0;
  bool maybe_continued = false, unfinished = true;
  string linbuf;
  fpos_t oldpos{};
  while( unfinished && fgetpos(file, &oldpos) == 0 &&
         (r = GetLine(file, buf, bufsiz, linbuf)) == 0 ) {
    // Search for comment or continuation character.
    // If found, remove it and everything that follows.
    bool continued = false, comment = false,
      trailing_space = false, leading_space = false, is_assignment = false;
    auto pos = linbuf.find_first_of("#\\");
    if( pos != string::npos ) {
      if( linbuf[pos] == '\\' )
        continued = true;
      else
        comment = true;
      linbuf.erase(pos);
    }
    // Trim leading and trailing space
    if( !linbuf.empty() ) {
      if( linbuf[0] == ' ' )
        leading_space = true;
      if( linbuf[linbuf.length() - 1] == ' ' )
        trailing_space = true;
      if( leading_space || trailing_space )
        Trim(linbuf);
    }

    if( line.empty() && linbuf.empty() )
      // Nothing to do, i.e. no line building in progress and no data
      continue;

    if( !linbuf.empty() ) {
      is_assignment = IsAssignment(linbuf);
      // Tentative continuation is canceled by a subsequent line with a '='
      if( maybe_continued && is_assignment ) {
        // We must have data at this point, so we can exit. However, the line
        // we've just read is obviously a good one, so we must also rewind the
        // file to the previous position so this line can be read again.
        assert(!line.empty());  // else maybe_continued not set correctly
        fsetpos(file, &oldpos);
        break;
      }
      // If the line has data, it isn't a comment, even if there was a '#'
      //      comment = false;  // not used
    } else if( continued || comment ) {
      // Skip empty continuation lines and comments in the middle of a
      // continuation block
      continue;
    } else {
      // An empty line, except for a comment or continuation, ends continuation.
      // Since we have data here, and this line is blank and would later be
      // skipped anyway, we can simply exit
      break;
    }

    if( line.empty() && !continued && is_assignment ) {
      // If the first line of a potential result contains a '=', this
      // line may be continued by non-'=' lines up until the next blank line.
      // However, do not use this logic if the line also contains a
      // continuation mark '\'; the two continuation styles should not be mixed
      maybe_continued = true;
    }
    unfinished = (continued || maybe_continued);

    // Ensure that at least one space is preserved between continuations,
    // if originally present
    if( maybe_continued || (trailing_space && continued) )
      linbuf += ' ';
    if( leading_space && !line.empty() && line[line.length() - 1] != ' ' )
      line += ' ';

    // Append current data to result
    line.append(linbuf);
  }

  // Because of the '=' sign continuation logic, we may have hit EOF if the last
  // line of the file is a key. In this case, we need to back out.
  if( maybe_continued ) {
    if( r == EOF ) {
      fsetpos(file, &oldpos);
      r = 0;
    }
    // Also, whether we hit EOF or not, tentative continuation may have
    // added a tentative space, which we tidy up here
    assert(!line.empty());
    if( line[line.length() - 1] == ' ' )
      line.erase(line.length() - 1);
  }
  return r;
}

//_____________________________________________________________________________
Int_t LoadDBvalue( FILE* file, const TDatime& date, const char* key, string& text )
{
  // Load a data value tagged with 'key' from the database 'file'.
  // Lines starting with "#" are ignored.
  // If 'key' is found, then the most recent value seen (based on time stamps
  // and position within the file) is returned in 'text'.
  // Values with time stamps later than 'date' are ignored.
  // This allows incremental organization of the database where
  // only changes are recorded with time stamps.
  // Return 0 if success, 1 if key not found, <0 if unexpected error.

  if( !file || !key ) return -255;
  TDatime keydate(950101, 0), prevdate(950101, 0);

  errno = 0;
  errtxt.clear();
  rewind(file);

  static const Int_t bufsiz = 256;
  char* buf = new char[bufsiz];

  bool found = false, do_ignore = false;
  string dbline;
  vector <string> lines;
  while( ReadDBline(file, buf, bufsiz, dbline) != EOF ) {
    if( dbline.empty() ) continue;
    // Replace text variables in this database line, if any. Multi-valued
    // variables are supported here, although they are only sensible on the LHS
    lines.assign(1, dbline);
    if( gHaTextvars )
      gHaTextvars->Substitute(lines);
    for( auto& line : lines ) {
      Int_t status = 0;
      if( !do_ignore && (status = IsDBkey(line, key, text)) != 0 ) {
        if( status > 0 ) {
          // Found a matching key for a newer date than before
          found = true;
          prevdate = keydate;
          // we do not set do_ignore to true here so that the _last_, not the first,
          // of multiple identical keys is evaluated.
        }
      } else if( IsDBdate(line, keydate) != 0 )
        do_ignore = (keydate > date || keydate < prevdate);
    }
  }
  delete[] buf;

  if( errno ) {
    perror("LoadDBvalue");
    return -1;
  }
  return found ? 0 : 1;
}

//_____________________________________________________________________________
Int_t LoadDBvalue( FILE* file, const TDatime& date, const char* key, Double_t& value )
{
  // Locate key in database, convert the text found to double-precision,
  // and return result in 'value'.
  // This is a convenience function.

  string text;
  Int_t err = LoadDBvalue(file, date, key, text);
  if( err == 0 )
    value = atof(text.c_str());
  return err;
}

//_____________________________________________________________________________
Int_t LoadDBvalue( FILE* file, const TDatime& date, const char* key, Int_t& value )
{
  // Locate key in database, convert the text found to integer
  // and return result in 'value'.
  // This is a convenience function.

  string text;
  Int_t err = LoadDBvalue(file, date, key, text);
  if( err == 0 )
    value = atoi(text.c_str());
  return err;
}

//_____________________________________________________________________________
Int_t LoadDBvalue( FILE* file, const TDatime& date, const char* key, TString& text )
{
  // Locate key in database, convert the text found to TString
  // and return result in 'text'.
  // This is a convenience function.

  string _text;
  Int_t err = LoadDBvalue(file, date, key, _text);
  if( err == 0 )
    text = _text.c_str();
  return err;
}

//_____________________________________________________________________________
template<class T>
Int_t LoadDBarray( FILE* file, const TDatime& date, const char* key, vector <T>& values )
{
  string text;
  Int_t err = LoadDBvalue(file, date, key, text);
  if( err )
    return err;
  values.clear();
  text += " ";
  istringstream inp(text);
  T dval;
  while( true ) {
    inp >> dval;
    if( inp.good() )
      values.push_back(dval);
    else
      break;
  }
  return 0;
}

//_____________________________________________________________________________
template<class T>
Int_t LoadDBmatrix( FILE* file, const TDatime& date, const char* key,
                    vector <vector<T>>& values, UInt_t ncols )
{
  // Read a matrix of values of type T into a vector of vectors.
  // The matrix is rectangular with ncols columns.

  vector<T> tmpval;
  Int_t err = LoadDBarray(file, date, key, tmpval);
  if( err ) {
    return err;
  }
  if( (tmpval.size() % ncols) != 0 ) {
    errtxt = "key = ";
    errtxt += key;
    return -129;
  }
  values.clear();
  typename vector<vector<T>>::size_type nrows = tmpval.size() / ncols, irow;
  for( irow = 0; irow < nrows; ++irow ) {
    vector<T> row;
    for( typename vector<T>::size_type i = 0; i < ncols; ++i ) {
      row.push_back(tmpval.at(i + irow * ncols));
    }
    values.push_back(row);
  }
  return 0;
}


//_____________________________________________________________________________
#define CheckLimits( T, val )                      \
  if( (val) < -std::numeric_limits<T>::max() ||    \
      (val) >  std::numeric_limits<T>::max() ) {   \
    ostringstream txt;                             \
    txt << (val);                                  \
    errtxt = txt.str();                            \
    goto rangeerr;                                 \
  }

#define CheckLimitsUnsigned( T, val )              \
  if( (val) < 0 || static_cast<ULong64_t>(val)     \
      > std::numeric_limits<T>::max() ) {          \
    ostringstream txt;                             \
    txt << (val);                                  \
    errtxt = txt.str();                            \
    goto rangeerr;                                 \
  }

//_____________________________________________________________________________
Int_t LoadDatabase( FILE* f, const TDatime& date, const DBRequest* req,
                    const char* prefix, Int_t search, const char* here )
{
  // Load a list of parameters from the database file 'f' according to
  // the contents of the 'req' structure (see VarDef.h).

  if( !req ) return -255;
  if( !prefix ) prefix = "";
  Int_t ret = 0;
  if( loaddb_depth++ == 0 )
    loaddb_prefix = prefix;

  const DBRequest* item = req;
  while( item->name ) {
    if( item->var ) {
      string keystr = prefix;
      keystr.append(item->name);
      UInt_t nelem = item->nelem;
      const char* key = keystr.c_str();
      if( item->type == kDouble || item->type == kFloat ) {
        if( nelem < 2 ) {
          Double_t dval = 0;
          ret = LoadDBvalue(f, date, key, dval);
          if( ret == 0 ) {
            if( item->type == kDouble )
              *((Double_t*)item->var) = dval;
            else {
              CheckLimits(Float_t, dval)
              *((Float_t*)item->var) = static_cast<Float_t>(dval);
            }
          }
        } else {
          // Array of reals requested
          vector<double> dvals;
          ret = LoadDBarray(f, date, key, dvals);
          if( ret == 0 && dvals.size() != nelem ) {
            nelem = dvals.size();
            ret = -130;
          } else if( ret == 0 ) {
            if( item->type == kDouble ) {
              for( UInt_t i = 0; i < nelem; i++ )
                ((Double_t*)item->var)[i] = dvals[i];
            } else {
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimits(Float_t, dvals[i])
                ((Float_t*)item->var)[i] = static_cast<Float_t>(dvals[i]);
              }
            }
          }
        }
      } else if( item->type >= kInt && item->type <= kByte ) {
        // Implies a certain order of definitions in VarType.h
        if( nelem < 2 ) {
          Int_t ival = 0;
          ret = LoadDBvalue(f, date, key, ival);
          if( ret == 0 ) {
            switch( item->type ) {
            case kInt:
              *((Int_t*)item->var) = ival;
              break;
            case kUInt:
              CheckLimitsUnsigned(UInt_t, ival)
              *((UInt_t*)item->var) = static_cast<UInt_t>(ival);
              break;
            case kShort:
              CheckLimits(Short_t, ival)
              *((Short_t*)item->var) = static_cast<Short_t>(ival);
              break;
            case kUShort:
              CheckLimitsUnsigned(UShort_t, ival)
              *((UShort_t*)item->var) = static_cast<UShort_t>(ival);
              break;
            case kChar:
              CheckLimits(Char_t, ival)
              *((Char_t*)item->var) = static_cast<Char_t>(ival);
              break;
            case kByte:
              CheckLimitsUnsigned(Byte_t, ival)
              *((Byte_t*)item->var) = static_cast<Byte_t>(ival);
              break;
            default:
              goto badtype;
            }
          }
        } else {
          // Array of integers requested
          vector <Int_t> ivals;
          ret = LoadDBarray(f, date, key, ivals);
          if( ret == 0 && ivals.size() != nelem ) {
            nelem = ivals.size();
            ret = -130;
          } else if( ret == 0 ) {
            switch( item->type ) {
            case kInt:
              for( UInt_t i = 0; i < nelem; i++ )
                ((Int_t*)item->var)[i] = ivals[i];
              break;
            case kUInt:
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimitsUnsigned(UInt_t, ivals[i])
                ((UInt_t*)item->var)[i] = static_cast<UInt_t>(ivals[i]);
              }
              break;
            case kShort:
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimits(Short_t, ivals[i])
                ((Short_t*)item->var)[i] = static_cast<Short_t>(ivals[i]);
              }
              break;
            case kUShort:
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimitsUnsigned(UShort_t, ivals[i])
                ((UShort_t*)item->var)[i] = static_cast<UShort_t>(ivals[i]);
              }
              break;
            case kChar:
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimits(Char_t, ivals[i])
                ((Char_t*)item->var)[i] = static_cast<Char_t>(ivals[i]);
              }
              break;
            case kByte:
              for( UInt_t i = 0; i < nelem; i++ ) {
                CheckLimitsUnsigned(Byte_t, ivals[i])
                ((Byte_t*)item->var)[i] = static_cast<Byte_t>(ivals[i]);
              }
              break;
            default:
              goto badtype;
            }
          }
        }
      } else if( item->type == kString ) {
        ret = LoadDBvalue(f, date, key, *((string*)item->var));
      } else if( item->type == kTString ) {
        ret = LoadDBvalue(f, date, key, *((TString*)item->var));
      } else if( item->type == kFloatV ) {
        ret = LoadDBarray(f, date, key, *((vector<float>*)item->var));
        if( ret == 0 && nelem > 0 && nelem !=
                                     static_cast<UInt_t>(((vector<float>*)item->var)->size()) ) {
          nelem = ((vector<float>*)item->var)->size();
          ret = -130;
        }
      } else if( item->type == kDoubleV ) {
        ret = LoadDBarray(f, date, key, *((vector<double>*)item->var));
        if( ret == 0 && nelem > 0 && nelem !=
                                     static_cast<UInt_t>(((vector<double>*)item->var)->size()) ) {
          nelem = ((vector<double>*)item->var)->size();
          ret = -130;
        }
      } else if( item->type == kIntV ) {
        ret = LoadDBarray(f, date, key, *((vector <Int_t>*)item->var));
        if( ret == 0 && nelem > 0 && nelem !=
                                     static_cast<UInt_t>(((vector <Int_t>*)item->var)->size()) ) {
          nelem = ((vector <Int_t>*)item->var)->size();
          ret = -130;
        }
      } else if( item->type == kFloatM ) {
        ret = LoadDBmatrix(f, date, key,
                           *((vector <vector<float>>*)item->var), nelem);
      } else if( item->type == kDoubleM ) {
        ret = LoadDBmatrix(f, date, key,
                           *((vector <vector<double>>*)item->var), nelem);
      } else if( item->type == kIntM ) {
        ret = LoadDBmatrix(f, date, key,
                           *((vector <vector<Int_t>>*)item->var), nelem);
      } else {
badtype:
        if( item->type >= kDouble && item->type <= kObject2P )
          ::Error(::Here(here, loaddb_prefix.c_str()),
                  R"(Key "%s": Reading of data type "%s" not implemented)",
                  key, Vars::GetEnumName(item->type));
        else
          ::Error(::Here(here, loaddb_prefix.c_str()),
                  R"/(Key "%s": Reading of data type "(#%d)" not implemented)/",
                  key, item->type);
        ret = -2;
        break;
rangeerr:
        ::Error(::Here(here, loaddb_prefix.c_str()),
                R"(Key "%s": Value %s out of range for requested type "%s")",
                key, errtxt.c_str(), Vars::GetEnumName(item->type));
        ret = -3;
        break;
      }

      if( ret == 0 ) {  // Key found -> next item
        goto nextitem;
      } else if( ret > 0 ) {  // Key not found
        // If searching specified, either for this key or globally, retry
        // finding the key at the next level up along the name tree. Name
        // tree levels are defined by dots (".") in the prefix. The top
        // level is 1 (where prefix = "").
        // Example: key = "nw", prefix = "L.vdc.u1", search = 1, then
        // search for:  "L.vdc.u1.nw" -> "L.vdc.nw" -> "L.nw" -> "nw"
        //
        // Negative values of 'search' mean search up relative to the
        // current level by at most abs(search) steps, or up to top level.
        // Example: key = "nw", prefix = "L.vdc.u1", search = -1, then
        // search for:  "L.vdc.u1.nw" -> "L.vdc.nw"

        // per-item search level overrides global one
        Int_t newsearch = (item->search != 0) ? item->search : search;
        if( newsearch != 0 && *prefix ) {
          string newprefix(prefix);
          Int_t newlevel = ChopPrefix(newprefix) + 1;
          if( newsearch < 0 || newlevel >= newsearch ) {
            DBRequest newreq[2];
            newreq[0] = *item;
            memset(newreq + 1, 0, sizeof(DBRequest));
            newreq->search = 0;
            if( newsearch < 0 )
              newsearch++;
            ret = LoadDatabase(f, date, newreq, newprefix.c_str(), newsearch, here);
            // If error, quit here. Error message printed at lowest level.
            if( ret != 0 )
              break;
            goto nextitem;  // Key found and ok
          }
        }
        if( item->optional )
          ret = 0;
        else {
          if( item->descript ) {
            ::Error(::Here(here, loaddb_prefix.c_str()),
                    R"/(Required key "%s" (%s) missing in the database.)/",
                    key, item->descript);
          } else {
            ::Error(::Here(here, loaddb_prefix.c_str()),
                    R"(Required key "%s" missing in the database.)", key);
          }
          // For missing keys, the return code is the index into the request
          // array + 1. In this way the caller knows which key is missing.
          ret = 1 + static_cast<Int_t>(std::distance(req, item));
          break;
        }
      } else if( ret == -128 ) {  // Line too long
        ::Error(::Here(here, loaddb_prefix.c_str()),
                "Text line too long. Fix the database!\n\"%s...\"",
                errtxt.c_str());
        break;
      } else if( ret == -129 ) {  // Matrix ncols mismatch
        ::Error(::Here(here, loaddb_prefix.c_str()),
                "Number of matrix elements not evenly divisible by requested "
                "number of columns. Fix the database!\n\"%s...\"",
                errtxt.c_str());
        break;
      } else if( ret == -130 ) {  // Vector/array size mismatch
        ::Error(::Here(here, loaddb_prefix.c_str()),
                "Incorrect number of array elements found for key = %s. "
                "%u requested, %u found. Fix database.", keystr.c_str(),
                item->nelem, nelem);
        break;
      } else {  // other ret < 0: unexpected zero pointer etc.
        ::Error(::Here(here, loaddb_prefix.c_str()),
                R"(Program error when trying to read database key "%s". )"
                "CALL EXPERT!", key);
        break;
      }
    }
nextitem:
    item++;
  }
  if( --loaddb_depth == 0 )
    loaddb_prefix.clear();

  return ret;
}

//_____________________________________________________________________________
Int_t SeekDBconfig( FILE* file, const char* tag, const char* label,
                    Bool_t end_on_tag )
{
  // Starting from the current position in 'file', look for the
  // configuration 'tag'. Position the file on the
  // line immediately following the tag. If no tag found, return to
  // the original position in the file.
  // Return zero if not found, 1 otherwise.
  //
  // Configuration tags have the form [ config=tag ].
  // If 'label' is given explicitly, it replaces 'config' in the tag string,
  // for example label="version" will search for [ version=tag ].
  // If 'label' is empty (""), search for just [ tag ].
  //
  // If 'end_on_tag' is true, quit if any non-matching tag found,
  // i.e. anything matching "*[*]*" except "[config=anything]".
  //
  // Useful for segmenting databases (esp. VDC) for different
  // experimental configurations.

  static const char* const here = "SeekDBconfig";

  if( !file || !tag || !*tag ) return 0;
  string _label("[");
  if( label && *label ) {
    _label.append(label);
    _label.append("=");
  }
  auto llen = _label.size();

  bool found = false;

  errno = 0;
  off_t pos = ftello(file);
  if( pos != -1 ) {
    bool quit = false;
    const int LEN = 256;
    char buf[LEN];
    while( !errno && !found && !quit && fgets(buf, LEN, file) ) {
      size_t len = strlen(buf);
      if( len < 2 || buf[0] == '#' ) continue;      //skip comments
      if( buf[len - 1] == '\n' ) buf[len - 1] = 0;     //delete trailing newline
      char* cbuf = ::Compress(buf);
      string line(cbuf);
      delete[] cbuf;
      auto lbrk = line.find(_label);
      if( lbrk != string::npos && lbrk + llen < line.size() ) {
        auto rbrk = line.find(']', lbrk + llen);
        if( rbrk == string::npos ) continue;
        if( line.substr(lbrk + llen, rbrk - lbrk - llen) == tag ) {
          found = true;
          break;
        }
      } else if( end_on_tag && IsTag(buf) )
        quit = true;
    }
  }
  if( errno ) {
    perror(here);
    found = false;
  }
  // If not found, rewind to previous position
  if( !found && pos >= 0 && fseeko(file, pos, SEEK_SET) )
    perror(here); // TODO: should throw exception

  return found;
}

//_____________________________________________________________________________
Int_t SeekDBdate( FILE* file, const TDatime& date, Bool_t end_on_tag )
{
  // Starting from the current position in file 'f', look for a
  // date tag matching time stamp 'date'. Position the file on the
  // line immediately following the tag. If no tag found, return to
  // the original position in the file.
  // Return zero if not found, 1 otherwise.
  // Date tags must be in SQL format: [ yyyy-mm-dd hh:mi:ss ].
  // If 'end_on_tag' is true, end the search at the next non-date tag;
  // otherwise, search through end of file.
  // Useful for sub-segmenting database files.

  static const char* const here = "SeekDBdateTag";

  if( !file ) return 0;
  const int LEN = 256;
  char buf[LEN];
  TDatime tagdate(950101, 0), prevdate(950101, 0);
  const bool kNoWarn = false;

  errno = 0;
  off_t pos = ftello(file);
  if( pos == -1 ) {
    if( errno )
      perror(here);
    return 0;
  }
  off_t foundpos = -1;
  bool found = false, quit = false;
  while( !errno && !quit && fgets(buf, LEN, file) ) {
    size_t len = strlen(buf);
    if( len < 2 || buf[0] == '#' ) continue;
    if( buf[len - 1] == '\n' ) buf[len - 1] = 0; //delete trailing newline
    string line(buf);
    if( IsDBdate(line, tagdate, kNoWarn)
        && tagdate <= date && tagdate >= prevdate ) {
      prevdate = tagdate;
      foundpos = ftello(file);
      found = true;
    } else if( end_on_tag && IsTag(buf) )
      quit = true;
  }
  if( errno ) {
    perror(here);
    found = false;
  }
  if( fseeko(file, (found ? foundpos : pos), SEEK_SET) ) {
    perror(here); // TODO: should throw exception
    found = false;
  }
  return found;
}

} // namespace Podd

