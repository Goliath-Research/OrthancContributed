#!/bin/bash
#
# Here is a backup script for PostgreSQL on UBUNTU. Please see the inline 
# comments for implementation:
#
# Orthanc backup script for PostgreSQL pg_backup.bash
# Based on code by "Fhilip and Pascal"
# http://www.defitek.com/blog/2010/01/06/a-simple-yet-effective-postgresql-backup-script/
# modified/fixed by J.Webster for Orthanc backup
#
# Crontab entry should besimilar to the below. Backup triggers at 30 min past midnight
# It must run under an account that has superuser privs to the database
# crontab will send output to the indicated
# 30 0 * * * /Orthanc/scripts/pg_backup.bash >> /Orthanc/scripts/dbbackup.log 2>&1
#
# Files should be placed at /Orthanc/scripts and execute privilege added
# e.g. chmod +x pg_backup.bash
#
logfile=/Orthanc/scripts/dbbackup.log     # => This must be the same as the crontab entry
backup_dir=/data/backups/postgres-backup/ # => location to put backup
backup_date=`date +%y%m%d%H%M`            # => string to append to the name of the backup files
number_of_days=7                          # => number of days to keep copies of databases

# Database names are chosen by any role with CREATEDB and are used to build the
# dump file path, so only names made of ASCII letters, digits and underscores
# are backed up.
is_safe_dbname() {
  local LC_ALL=C
  [[ $1 =~ ^[A-Za-z0-9_]+$ ]]
}

# Dumps hold the whole database: keep them readable by the backup account only
umask 077

# log the starting time
echo [`date`] PostgreSQL backup START `date`

# Backup each database, but exclude templates and postgres d'base.
# Names are read NUL-separated so that no character in a name can split it.
while IFS= read -r -d '' i; do
  if ! is_safe_dbname "$i"; then
    printf ' –> Skipping database with unsafe name %q\n' "$i"
    continue
  fi
  outfile="${backup_dir}${i}-pg_dump.${backup_date}.gz"
  echo -n " –> Dumping $i to $outfile "
  pg_dump -Fc --dbname="$i" | gzip > "$outfile"
  outfilesize=`ls -lh "$outfile" | awk '{ print $5 }'`
  echo  " ($outfilesize) "
done < <(psql -X -d postgres -A -t -0 -c "SELECT datname FROM pg_catalog.pg_database WHERE datallowconn AND NOT datistemplate AND datname <> 'postgres' ORDER BY datname")

# Delete old backups and trim the backup log file
echo " –> Deleting backups older than $number_of_days days "
find $backup_dir -type f -prune -mtime +$number_of_days -exec rm -f {} \;
# finish time
echo [`date`] PostgreSQL backup END `date`
# Trim the log file
# Ensure this is the same name as the contab
tail -150 $logfile > trimmed.log
mv trimmed.log $logfile
echo "Logfile trimmed" >>$logfile
exit
