//////////////////////////////////////////////////////////////////
//
//    FreeLing - Open Source Language Analyzers
//
//    Copyright (C) 2014   TALP Research Center
//                         Universitat Politecnica de Catalunya
//
//    This library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Affero General Public
//    License as published by the Free Software Foundation; either
//    version 3 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public
//    License along with this library; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//
//    contact: Lluis Padro (padro@lsi.upc.es)
//             TALP Research Center
//             despatx C6.212 - Campus Nord UPC
//             08034 Barcelona.  SPAIN
//
////////////////////////////////////////////////////////////////

#include <fstream>
#include <sstream>
#include <vector>

#include "freeling/morfo/traces.h"
#include "freeling/morfo/util.h"
#include "freeling/morfo/dictionary.h"
#include "freeling/morfo/configfile.h"
#include "freeling/morfo/alternatives.h"

using namespace std;

namespace freeling {

#define MOD_TRACENAME L"ALTERNATIVES"
#define MOD_TRACECODE ALTERNATIVES_TRACE


  ///////////////////////////////////////////////////////////////
  ///  Create a alternatives module, loading dictionary and options
  ///////////////////////////////////////////////////////////////

  alternatives::alternatives(const std::wstring &altsFile) : CheckKnownTags(L"^$") {
 
    wstring path=altsFile.substr(0,altsFile.find_last_of(L"/\\")+1);
 
    // default
    DistanceThreshold=2;
    MaxSizeDiff=3;
    CheckUnknown=true;
    RemoveRepeated=0;
    sed=NULL;
    comp=NULL;

    enum sections {GENERAL,DISTANCE,TARGET};
    config_file cfg(true);
    cfg.add_section(L"General",GENERAL);
    cfg.add_section(L"Distance",DISTANCE);
    cfg.add_section(L"Target",TARGET);

    if (not cfg.open(altsFile))
      ERROR_CRASH(L"Error opening file "+altsFile);

    wstring dic_file, ph_file, cost_file;
    int dic_type=0;
    bool compounds=false;;

    // load ortographic alternatives configuration
    wstring line;
    while (cfg.get_content_line(line)) {
      wistringstream sin;
      sin.str(line);
      wstring key,value;
      sin>>key>>value;

      switch (cfg.get_section()) {
        // Read <General> section contents
      case GENERAL: {
        if (key==L"Type") {
          if (freeling::util::lowercase(value)==L"orthographic") DistanceType=ORTHOGRAPHIC;
          else if (freeling::util::lowercase(value)==L"phonetic") DistanceType=PHONETIC;
          else ERROR_CRASH(L"Unexpected value '"+value+L"' for 'Type' in file "+altsFile);
        }
        else if (key==L"Dictionary") {
          if (dic_type!=0) 
            ERROR_CRASH(L"More than one Dictionary specified in file "+altsFile);
          dic_type = ORTHOGRAPHIC;
          dic_file = util::absolute(value,path); 
        }
        else if (key==L"PhoneticDictionary") {
          if (dic_type!=0) 
            ERROR_CRASH(L"More than one Dictionary specified in file "+altsFile);
          dic_type = PHONETIC;
          dic_file = util::absolute(value,path); 
        }
        else if (key==L"PhoneticRules") 
          ph_file = util::absolute(value,path); 
        else if (key==L"Compounds") 
          compounds = (value==L"yes" or value==L"y");
        else 
          WARNING(L"Ignoring unexpected line '"+line+L"' in file "+altsFile);
        break;
      }

        // Read <Distance> section contents
      case DISTANCE: {
        if (key==L"CostMatrix") 
          cost_file = util::absolute(value,path); 
        else if (key==L"Threshold") 
          DistanceThreshold = freeling::util::wstring2int(value);
        else if (key==L"MaxSizeDiff") 
          MaxSizeDiff = freeling::util::wstring2int(value);
        else 
          WARNING(L"Ignoring unexpected line '"+line+L"' in file "+altsFile);
        break;
      }

        // Read <Target> section contents
      case TARGET: {
        if (key==L"UnknownWords")  {
          value=util::lowercase(value);
          CheckUnknown = (value==L"yes" or value==L"y");
        }
        else if (key==L"KnownWords") {
          if (value!=L"none" and value!=L"no") CheckKnownTags = freeling::regexp(value);
        }
        else if (key==L"RemoveRepeated") 
          RemoveRepeated = freeling::util::wstring2int(value);
        else 
          WARNING(L"Ignoring unexpected line '"+line+L"' in file "+altsFile);
        break;
      }
      default: break;
      }
    }

    cfg.close();

    if (dic_type==0) ERROR_CRASH(L"No dictionary specified.");
    if (dic_type==PHONETIC and DistanceType!=PHONETIC) ERROR_CRASH(L"Only 'Type phonetic' is allowed with PhoneticDictionary.");
    if ((dic_type==PHONETIC) == (ph_file.empty())) ERROR_CRASH(L"PhoneticRules must be specified iff PhoneticDictionary is used.");

    /// Create phonetic transcriptor
    ph = NULL;
    if (not ph_file.empty()) {
      TRACE(3,L"Creating phonetic encoder");
      ph = new phonetics(ph_file);
    }

    wstringstream dic_buff;
    if (DistanceType==PHONETIC) {

      // open given dictionary
      wifstream fdic;
      util::open_utf8_file(fdic,dic_file);
      if (fdic.fail()) ERROR_CRASH(L"Error opening file "+dic_file);  

      wstring line, key, form, sound;
      while (getline(fdic,line)) {
        wistringstream sin; sin.str(line); 
        // get sound
        sin>>key;

        if (dic_type==PHONETIC) 
          // dictionary is already phonetic, just load forms for read sound key
          while (sin>>form) 
            orthography.insert(make_pair(key,form));

        else if (dic_type==ORTHOGRAPHIC) {
          // dictionary is orthographic. Encode key, and store pair sound-key
          sound = ph->get_sound(key);  // compute sound
          dic_buff << sound << endl;     // write sound to temp buffer dictionary
          orthography.insert(make_pair(sound,key)); // remember which word sounds like this
        }
      }      
      fdic.close();
    }

    // create FSM 
    TRACE(3,L"Creating foma FSMs for SED");

    bool phon_converted = (DistanceType==PHONETIC and dic_type==ORTHOGRAPHIC);
    // distance is phonetic but dictionary was orthographic.  Use converted dictionary in dic_buff
    if (phon_converted) sed = new foma_FSM(dic_buff, cost_file);  
    // distance and dictionary are of the same kind, use given file straightforwardly
    else sed = new foma_FSM(dic_file, cost_file);
    sed->set_cutoff_threshold(DistanceThreshold);

    if (compounds) {
      TRACE(3,L"Creating foma FSMs for COMP");
      list<wstring> joins={L"-"};
      if (phon_converted) comp = new foma_FSM(dic_buff, cost_file, joins);
      else comp = new foma_FSM(dic_file, cost_file, joins);
      comp->set_cutoff_threshold(DistanceThreshold);
    }
    
    TRACE(1,L"alternatives succesfully created");
  }



  ////////////////////////////////////////////////
  /// Destroy alternatives module, close database, delete objects.
  ////////////////////////////////////////////////

  alternatives::~alternatives(){
    // delete phonetics module
    delete ph;
    // delete Foma FSA
    delete sed;
    // delete compounds Foma FSA
    delete comp;
  }


  ////////////////////////////////////////////////
  /// filter given candidate and decide if it is a valid alternative.
  ////////////////////////////////////////////////

  void alternatives::filter_candidate(const wstring &form, const wstring &candidate, int distance, map<wstring,int> & filtered) const {

    // If the alternate form is different than the actual, add it.
    // (it might be equal if we searched for a known word)
    TRACE(3,L"     Filtering candidate "+candidate);
    if (candidate!=form && labs(form.size()-candidate.size())<=MaxSizeDiff) {
      map<wstring,int>::iterator p=filtered.find(candidate);
      if (p==filtered.end()) {  
        // if candidate is new, add it
        filtered.insert(make_pair(candidate,distance));
        TRACE(3,L"     - added alternative ("+candidate+L","+util::int2wstring(distance)+L")"); 
      }
      else {
        // if candidate is not new, keep minimum distance
        p->second = min(p->second,distance);
        TRACE(3,L"     - updated alternative distance ("+candidate+L","+util::int2wstring(p->second)+L")"); 
      }
    }
  }


  ////////////////////////////////////////////////////////////////////////
  /// sort criteria: shortests edit distance, if equal smaller length difference
  ////////////////////////////////////////////////////////////////////////

  inline bool compare_alternatives(const pair<wstring,pair<int,int> > &p1,
                                   const pair<wstring,pair<int,int> > &p2) {
    return p1.second.first<p2.second.first or 
      (p1.second.first==p2.second.first and p1.second.second<p2.second.second) or
      (p1.second.first==p2.second.first and p1.second.second==p2.second.second and p1.first<p2.first);
  }

  ////////////////////////////////////////////////////////////////////////
  /// adds the new words that are valid alternatives.
  ////////////////////////////////////////////////////////////////////////

  void alternatives::filter_alternatives(const list<alternative> &alts, word &w) const {

    // if we are doing phonetic match, and there is an exact phonetic match,
    // lower the threshold for filtering (i.e. be pickier for non-exact matches)
    int max = DistanceThreshold;
    if (DistanceType==PHONETIC and not alts.empty() and alts.begin()->get_distance()==0) 
      max = max/2;

    // put toghether existing alternatives (if any) plus new ones
    w.get_alternatives().insert(w.get_alternatives().end(), alts.begin(), alts.end());

    // sort alphabetically, then by distance
    w.get_alternatives().sort([](const freeling::alternative &left,
                                 const freeling::alternative &right) {
                                   if (left.get_form() == right.get_form()) 
                                     return left.get_distance() < right.get_distance();
                                   else 
                                     return left.get_form()<right.get_form();}
                              );
    // remove duplicates
    w.get_alternatives().unique([](const freeling::alternative &left,
                                   const freeling::alternative &right) {
                                  return left.get_form() == right.get_form();}
                                );
    // sort by distance, then by length difference 
    w.get_alternatives().sort([&](const freeling::alternative &left,
                                 const freeling::alternative &right) {
                                if (left.get_distance() == right.get_distance() ) {
                                  int dL = labs(left.get_form().length()-w.get_form().length());
                                  int dR = labs(right.get_form().length()-w.get_form().length());
                                  return dL<dR;
                                }
                                else 
                                  return left.get_distance() < right.get_distance(); }
                              );

    w.get_alternatives().remove_if([&](const freeling::alternative &alt) {return alt.get_distance()>max;} );

      /*    
    // start with existing alternatives (if any)
    map<wstring,int> filtered;
    for (list<freeling::alternative>::const_iterator a=w.alternatives_begin(); a!=w.alternatives_end(); a++) 
      filtered.insert(make_pair(a->get_form(), a->get_distance()));

    // if we are doing phonetic match, and there is an exact phonetic match,
    // lower the threshold for filtering (i.e. be pickier for non-exact matches)
    int max = DistanceThreshold;
    if (DistanceType==PHONETIC and not alts.empty() and alts.begin()->second==0) 
      max = max/2;

    // select alternatives suggested by distance
    TRACE(3,L"   Filtering alternatives ");
    for (list<pair<wstring,int> >::const_iterator a=alts.begin(); a!=alts.end() && a->second<=max; a++) 
      filter_candidate(w.get_lc_form(), a->first, a->second, filtered);

    /// auxiliary list to sort alternatives by edit distance + length difference
    list<pair<wstring,pair<int,int> > > aux;
    for (map<wstring,int>::const_iterator f=filtered.begin(); f!=filtered.end(); f++) 
      aux.push_back(make_pair(f->first,make_pair(f->second,labs(f->first.size()-w.get_form().size()))));

    aux.sort(compare_alternatives);
    
    // add selected alternatives to the word 
    w.clear_alternatives();
    for (list<pair<wstring,pair<int,int> > >::const_iterator f=aux.begin(); f!=aux.end(); f++) 
      // add new pair form-distance to final alternatives list.
      w.get_alternatives().push_back(freeling::alternative(f->first,f->second.first));
      */
  }





  ////////////////////////////////////////////////////////////////////////
  /// Navigates the sentence adding alternative words (possible correct spelling data)
  ////////////////////////////////////////////////////////////////////////

  void alternatives::analyze(sentence &se) const {

    for (sentence::iterator pos=se.begin(); pos!=se.end(); ++pos)  {    

      bool check = false;
      if (pos->get_n_analysis()) { // Known word, check if some tag matches CheckKnownTags
        for (word::iterator a=pos->begin(); a!=pos->end() and not check; a++) 
          check = CheckKnownTags.search(a->get_tag());
      }
      else 
        check = CheckUnknown;  // unknown word. Check it if config file requires it 
                      
      TRACE(3,L"Checking word "+pos->get_form()+L": "+(check?L"yes":L"no"));

      if (check) {

        // if removeRepeated is active, remove repeated characters
        wstring form = pos->get_lc_form();
        wstring no_repeated = pos->get_lc_form();
        if (RemoveRepeated>0) {
          // if the number of removed chars is larger than threshold, 
          // use only shortened version of the word
          no_repeated = remove_repeated(form);
          if (form.length() - no_repeated.length() > RemoveRepeated ) 
            form = no_repeated;
        }

        // look for alternatives
        TRACE(3,L"get alternatives for "+form);
        list<alternative> alts;
        get_similar_words(form, alts);
        TRACE(4,L" found "+util::int2wstring(alts.size())+L" alternatives");

        // if number of removed chars is  under the threshold
        // we have looked for the original word.  If corrections were found,
        // (and num of removed chars is not zero, i.e. words differ) add 
        // corrections for reduced word.
        if (not alts.empty() and form.length()!=no_repeated.length()) {
          TRACE(3,L"get alternatives for "+no_repeated);
          get_similar_words(no_repeated, alts);
          TRACE(4,L" found total of "+util::int2wstring(alts.size())+L" alternatives");
        }
        
        // filter and add the obtained words as new analysis
        if (not alts.empty()) filter_alternatives(alts,*pos);
      }
    }  
  }


  ////////////////////////////////////////////////////////////////////////
  /// Given a list of sounds, retrieve all possible lists of words
  /// that match them (one-to-one)
  ////////////////////////////////////////////////////////////////////////

  list<wstring> alternatives::recover_words(list<wstring> wds) const {

    list<wstring> res;

    if (wds.size()==1) {     
      typedef multimap<wstring,wstring>::const_iterator mm_it;
      pair<mm_it,mm_it> m = orthography.equal_range(*wds.begin());
      for (mm_it p=m.first; p!=m.second; p++) 
        res.push_back(p->second);          
    }
    
    else if (wds.size()>1) {         
      wstring h = *wds.begin();
      wds.pop_front();
      wds = recover_words(wds);

      typedef multimap<wstring,wstring>::const_iterator mm_it;
      pair<mm_it,mm_it> m = orthography.equal_range(h);
      for (mm_it p=m.first; p!=m.second; p++) 
        for (list<wstring>::iterator r=wds.begin(); r!=wds.end(); r++)
          res.push_back(p->second + L"_" + *r);                     
    }

    return res;
  }


  ////////////////////////////////////////////////////////////////////////
  /// Provide direct access to results of underlying automata, in case 
  /// caller only want the list of strings
  ////////////////////////////////////////////////////////////////////////

  void alternatives::get_similar_words(const wstring &form, list<alternative> & results) const {

    if (DistanceType==ORTHOGRAPHIC) {
      TRACE(4,L"Using ORTHO ");
      sed->get_similar_words(form, results);
      if (comp!=NULL) comp->get_similar_words(form, results);
    }

    else if (DistanceType==PHONETIC) {
      TRACE(4,L"Using PHON ");
      // encode given word
      wstring phon = ph->get_sound(form);
      TRACE(4,L" Get words similar to "+form+L".  Looking for entries sounding like "+phon);
      list<alternative> aux;

      // get similar sounds
      sed->get_similar_words(phon, aux);
      if (comp!=NULL) comp->get_similar_words(phon, aux);

      set<wstring> seen;
      for (list<alternative>::const_iterator a=aux.begin(); a!=aux.end(); a++) {
        TRACE(5,L"   Found similar sound "+a->get_form());

        list<wstring> wds = util::wstring2list(a->get_form(),L"_");
        list<wstring> orts = recover_words(wds);        
        for (list<wstring>::iterator w=orts.begin(); w!=orts.end(); w++) {
          TRACE(5,L"      corresponding words "+*w);
          if (labs(form.size()-w->size())<=MaxSizeDiff && seen.find(*w)==seen.end()) {
            TRACE(5,L"         added ");
            results.push_back(alternative(*w,a->get_distance()));
            seen.insert(*w);
          }
        }
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////
  /// Removes repeated consuecutive characters of a string
  /// Examples:  hoolaaaa => hola, VAMOOOOS => VAMOS,
  ///            aaa => a,  COMOooOo? => COMOoOo?
  ////////////////////////////////////////////////////////////////////////

  wstring alternatives::remove_repeated(const wstring &str) const {
    wstring res = str;
    unsigned int i = res.length() - 1;
    while (i > 0) {
      if (res[i] == res[i-1]) res.erase(i-1, 1);
      --i;
    }
    return res;
  }

} // namespace
