/*
  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
  License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with this program.
  If not, see http://www.gnu.org/licenses/
*/
#ifndef rdmFIFO_h
#define rdmFIFO_h

#include "rdm.h"
#include "rdmDataTypes.h"

#define RDM_MAX_RDM_QUEUE 30

class rdmFIFO {
  public:
    rdmFIFO();
    void init();
    bool push(rdm_data* a);
    rdm_data* peek(void);
    bool pop(rdm_data* a);
    bool isEmpty(void);
    bool notEmpty(void);
    bool isFull(void);
    uint8_t count(void);
    uint8_t space(void);
    void empty(void);

  private:
    rdm_data* resize(uint8_t s);

    rdm_data* content[RDM_MAX_RDM_QUEUE];
    uint8_t RDMfifoMaxSize;
    uint8_t RDMfifoAllocated;
    uint8_t RDMfifoSize;
};


#endif
